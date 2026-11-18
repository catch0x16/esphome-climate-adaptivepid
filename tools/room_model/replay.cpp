// Replays recorded Home Assistant history through the room model the units run
// (components/climate_adaptivepid/room_model.h), to see what a unit would learn
// from it and whether the output estimate holds up.
//
// Build:
//   c++ -std=c++17 -O2 -Wall -Wextra -Icomponents/climate_adaptivepid -o /tmp/replay tools/room_model/replay.cpp
//
// Input: a CSV downloaded from Home Assistant's History panel ("Download data")
// with these entities in it, columns entity_id,state,last_changed:
//   climate.<device>_hvac                    mode: heat / cool (anything else: not learned)
//   sensor.<device>_room_temperature         room temperature
//   sensor.<device>_outside_temperature      outdoor temperature
//   sensor.<device>_set_point                setpoint the unit was given
//   binary_sensor.<device>_operating         unit actively running
//   binary_sensor.<device>_internal_power_on what the workflow commanded
//
// Run:
//   /tmp/replay history.csv --device mainfloorhvac --fahrenheit
// Options:
//   --device NAME            entity prefix (the ESPHome device name)
//   --climate/--room/--outdoor/--setpoint/--operating/--power ENTITY_ID
//                            override one entity id
//   --fahrenheit             temperatures in the export are in F
//   --full-output-gap X      as configured on the unit (default 0.75)
//   --dead-time-min X        starting dead time (default 5)
//   --kp X                   starting rates from kp, as the unit does without initial rates
//
// The CSV has states only, not attributes, so defrost (hvac_action) is not
// visible here: defrost windows can only be caught as anomalies.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "room_model.h"
#include "tuning_rule.h"

using workflow::pid::ModelSample;
using workflow::pid::ModelState;
using workflow::pid::RoomModel;
using workflow::pid::TuningConfig;

struct Change {
  double t;  // seconds since the epoch
  std::string state;
};

// "2026-11-14T08:00:00.123Z" or "...+00:00" to seconds since the epoch.
static bool parse_time(const std::string &text, double &out) {
  int year, month, day, hour, minute;
  double second;
  int consumed = 0;
  if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%lf%n", &year, &month, &day, &hour, &minute, &second, &consumed) != 6) {
    return false;
  }
  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  double offset = 0.0;
  const char *zone = text.c_str() + consumed;
  if (*zone == '+' || *zone == '-') {
    int zh = 0, zm = 0;
    std::sscanf(zone + 1, "%d:%d", &zh, &zm);
    offset = (zh * 3600 + zm * 60) * (*zone == '-' ? -1.0 : 1.0);
  }
  out = static_cast<double>(timegm(&tm)) + second - offset;
  return true;
}

static std::string unquote(std::string s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
  while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

// The state in force at `t`, walking `index` forward through time.
static const std::string *state_at(const std::vector<Change> &changes, size_t &index, double t) {
  while (index + 1 < changes.size() && changes[index + 1].t <= t) index++;
  if (changes.empty() || changes[index].t > t) return nullptr;
  return &changes[index].state;
}

static float number(const std::string *state, bool fahrenheit) {
  if (state == nullptr) return NAN;
  char *end = nullptr;
  const float value = std::strtof(state->c_str(), &end);
  if (end == state->c_str()) return NAN;  // unknown, unavailable
  return fahrenheit ? (value - 32.0f) * 5.0f / 9.0f : value;
}

static void print_model(const char *label, const RoomModel &model, const TuningConfig &config) {
  const ModelState &m = model.state();
  const auto heat = workflow::pid::gains_from_rate(m.heat_rate, m.heat_dead_s, config);
  const auto cool = workflow::pid::gains_from_rate(m.cool_rate, m.cool_dead_s, config);
  std::printf("%s\n", label);
  std::printf("  heat loss       %.3f /h        (%u idle windows)\n", m.loss * 3600.0f, model.loss_measurements());
  std::printf("  heating rate    %.2f C/h       (%u windows)  dead time %.1f min (%u starts)  kp %.3f ki %.6f\n",
              m.heat_rate * 3600.0f, model.rate_measurements(true), m.heat_dead_s / 60.0f,
              model.dead_time_measurements(true), heat.kp, heat.ki);
  std::printf("  cooling rate    %.2f C/h       (%u windows)  dead time %.1f min (%u starts)  kp %.3f ki %.6f\n",
              m.cool_rate * 3600.0f, model.rate_measurements(false), m.cool_dead_s / 60.0f,
              model.dead_time_measurements(false), cool.kp, cool.ki);
  std::printf("  anomalies       %u windows dropped\n", model.anomalies());
}

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s history.csv --device NAME [--fahrenheit] [options, see the source]\n", argv[0]);
    return EXIT_FAILURE;
  }
  std::string device;
  std::map<std::string, std::string> entity;  // role -> entity id
  bool fahrenheit = false;
  float kp = 0.0f;
  TuningConfig config;
  for (int i = 2; i < argc; i++) {
    const std::string arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--fahrenheit") {
      fahrenheit = true;
    } else if (arg == "--device" && has_value) {
      device = argv[++i];
    } else if (arg == "--full-output-gap" && has_value) {
      config.full_output_gap = std::strtof(argv[++i], nullptr);
    } else if (arg == "--dead-time-min" && has_value) {
      config.dead_time_s = std::strtof(argv[++i], nullptr) * 60.0f;
    } else if (arg == "--kp" && has_value) {
      kp = std::strtof(argv[++i], nullptr);
    } else if (arg.rfind("--", 0) == 0 && has_value) {
      entity[arg.substr(2)] = argv[++i];
    } else {
      std::fprintf(stderr, "unknown option %s\n", arg.c_str());
      return EXIT_FAILURE;
    }
  }
  const std::map<std::string, std::string> defaults = {
      {"climate", "climate." + device + "_hvac"},
      {"room", "sensor." + device + "_room_temperature"},
      {"outdoor", "sensor." + device + "_outside_temperature"},
      {"setpoint", "sensor." + device + "_set_point"},
      {"operating", "binary_sensor." + device + "_operating"},
      {"power", "binary_sensor." + device + "_internal_power_on"},
  };
  for (const auto &it : defaults) {
    if (entity.count(it.first) == 0) entity[it.first] = it.second;
  }

  std::ifstream in(argv[1]);
  if (!in) {
    std::fprintf(stderr, "cannot read %s\n", argv[1]);
    return EXIT_FAILURE;
  }
  std::map<std::string, std::vector<Change>> history;  // entity id -> changes
  std::string line;
  std::getline(in, line);  // header
  while (std::getline(in, line)) {
    std::stringstream row(line);
    std::string id, state, when;
    if (!std::getline(row, id, ',') || !std::getline(row, state, ',') || !std::getline(row, when)) continue;
    Change change;
    if (!parse_time(unquote(when), change.t)) continue;
    change.state = unquote(state);
    history[unquote(id)].push_back(change);
  }
  for (auto &it : history) {
    std::stable_sort(it.second.begin(), it.second.end(), [](const Change &a, const Change &b) { return a.t < b.t; });
  }
  double start = 0.0, end = 0.0;
  for (const auto &role : defaults) {
    const auto found = history.find(entity[role.first]);
    if (found == history.end() || found->second.empty()) {
      std::fprintf(stderr, "no history for %s (%s); use --%s to name it\n", entity[role.first].c_str(),
                   role.first.c_str(), role.first.c_str());
      return EXIT_FAILURE;
    }
    start = std::max(start, found->second.front().t);
    end = end == 0.0 ? found->second.back().t : std::max(end, found->second.back().t);
  }

  // The same seeds as a unit without initial rates; then three models:
  // all windows (what the unit learns), and full and partial output only,
  // whose heating rates should agree if the output estimate is right.
  ModelState seed{RoomModel::DEFAULT_LOSS, workflow::pid::DEFAULT_RATE, workflow::pid::DEFAULT_RATE,
                  config.dead_time_s, config.dead_time_s};
  if (kp > 0.0f) {
    seed.heat_rate = seed.cool_rate = workflow::pid::rate_from_kp(kp, config);
  }
  RoomModel unit, full, partial;
  for (RoomModel *m : {&unit, &full, &partial}) m->set_state(seed);
  full.set_output_band(0.9f, 1.0f);
  partial.set_output_band(RoomModel::MIN_OUTPUT, 0.9f);

  std::map<std::string, size_t> cursor;
  const double step = 10.0;
  uint64_t learned_samples = 0;
  for (double t = start; t <= end; t += step) {
    const std::string *mode = state_at(history[entity["climate"]], cursor["climate"], t);
    const bool heat = mode != nullptr && *mode == "heat";
    const bool cool = mode != nullptr && *mode == "cool";
    if (!heat && !cool) continue;  // the workflows only run in heat and cool
    const std::string *operating = state_at(history[entity["operating"]], cursor["operating"], t);
    const std::string *power = state_at(history[entity["power"]], cursor["power"], t);
    const bool running = operating != nullptr && *operating == "on" && power != nullptr && *power == "on";
    const float room = number(state_at(history[entity["room"]], cursor["room"], t), fahrenheit);
    const float outdoor = number(state_at(history[entity["outdoor"]], cursor["outdoor"], t), fahrenheit);
    const float setpoint = number(state_at(history[entity["setpoint"]], cursor["setpoint"], t), fahrenheit);
    const float output = workflow::pid::estimate_output(running, heat, setpoint, room, config.full_output_gap);
    const ModelSample sample{static_cast<uint32_t>(static_cast<uint64_t>((t - start) * 1000.0)), room, outdoor,
                             heat, running, output};
    for (RoomModel *m : {&unit, &full, &partial}) m->update(sample);
    learned_samples++;
  }

  std::printf("%.1f days of history, %.1f days in heat or cool\n\n", (end - start) / 86400.0,
              learned_samples * step / 86400.0);
  print_model("Learned from all windows (what the unit would learn):", unit, config);
  std::printf("\nOutput estimate check (heating and cooling rates should agree):\n");
  std::printf("  full output (0.9-1.0):    heating %.2f C/h (%u windows), cooling %.2f C/h (%u windows)\n",
              full.rate(true) * 3600.0f, full.rate_measurements(true), full.rate(false) * 3600.0f,
              full.rate_measurements(false));
  std::printf("  partial output (0.3-0.9): heating %.2f C/h (%u windows), cooling %.2f C/h (%u windows)\n",
              partial.rate(true) * 3600.0f, partial.rate_measurements(true), partial.rate(false) * 3600.0f,
              partial.rate_measurements(false));
  return EXIT_SUCCESS;
}
