#pragma once

#include <stdint.h>

namespace mqtt_announcement {

enum class Kind : uint8_t {
  None,
  RemoteDiscovery,
  RemoteState,
  RemoteCounter,
  ControlConfig,
  ControlDiscovery,
  BridgeDiscovery,
  Health,
  Availability,
};

struct Step {
  Kind kind;
  uint8_t index;
  uint8_t part;
};

class Sequence {
 public:
  void start(uint8_t remotes, uint8_t controls) {
    _remotes = remotes;
    _controls = controls;
    _step = {Kind::RemoteDiscovery, 0, 0};
    skipEmptyGroups();
  }

  bool active() const { return _step.kind != Kind::None; }
  Step current() const { return _step; }
  void stop() { _step = {Kind::None, 0, 0}; }

  // Returns true only after Availability, the final step, has succeeded.
  bool advance() {
    switch (_step.kind) {
      case Kind::RemoteDiscovery:
        advanceParts(3, Kind::RemoteState);
        break;
      case Kind::RemoteState:
        advanceParts(3, Kind::RemoteCounter);
        break;
      case Kind::RemoteCounter:
        advanceItems(_remotes, Kind::ControlConfig);
        break;
      case Kind::ControlConfig:
        advanceItems(_controls, Kind::ControlDiscovery);
        break;
      case Kind::ControlDiscovery:
        advanceItems(_controls, Kind::BridgeDiscovery);
        break;
      case Kind::BridgeDiscovery:
        if (++_step.part >= 10) {
          _step = {Kind::Health, 0, 0};
        }
        break;
      case Kind::Health:
        _step = {Kind::Availability, 0, 0};
        break;
      case Kind::Availability:
        stop();
        return true;
      case Kind::None:
        return false;
    }
    skipEmptyGroups();
    return false;
  }

 private:
  Step _step = {Kind::None, 0, 0};
  uint8_t _remotes = 0;
  uint8_t _controls = 0;

  void advanceParts(uint8_t parts, Kind next) {
    if (++_step.part < parts) {
      return;
    }
    _step.part = 0;
    if (++_step.index < _remotes) {
      return;
    }
    _step = {next, 0, 0};
  }

  void advanceItems(uint8_t count, Kind next) {
    if (++_step.index >= count) {
      _step = {next, 0, 0};
    }
  }

  void skipEmptyGroups() {
    while ((_step.kind == Kind::RemoteDiscovery || _step.kind == Kind::RemoteState ||
            _step.kind == Kind::RemoteCounter) &&
           _remotes == 0) {
      _step = {_step.kind == Kind::RemoteDiscovery
                   ? Kind::RemoteState
                   : _step.kind == Kind::RemoteState ? Kind::RemoteCounter
                                                     : Kind::ControlConfig,
               0, 0};
    }
    while ((_step.kind == Kind::ControlConfig ||
            _step.kind == Kind::ControlDiscovery) &&
           _controls == 0) {
      _step = {_step.kind == Kind::ControlConfig ? Kind::ControlDiscovery
                                                : Kind::BridgeDiscovery,
               0, 0};
    }
  }
};

}  // namespace mqtt_announcement
