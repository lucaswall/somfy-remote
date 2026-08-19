#pragma once

#include <PubSubClient.h>
#include <WiFiClient.h>

#include "config_doc.h"
#include "control_map.h"
#include "receiver.h"
#include "record_store.h"
#include "remotes.h"
#include "store.h"

// The only place that knows Home Assistant exists, and where the recovery story lives: HA
// holds the configuration and a mirror of every rolling code, reconciled at boot under one
// rule — **the mirror is a floor in both directions.** HA may raise a counter on a board
// that is behind; the board may never lower one.
class HaMqtt {
 public:
  HaMqtt(Remotes &remotes, Store &store, Receiver &receiver, const char *clientId)
      : _mqtt(_wifi), _remotes(remotes), _store(store), _receiver(receiver),
        _clientId(clientId) {}

  void loop();
  bool connected() { return _mqtt.connected(); }

  // True once a configuration has been applied from either side. Until then the device
  // controls nothing and says so.
  bool configured() const { return _remotes.count() > 0; }

  // Display only. Empty when Home Assistant has not published a name for this remote,
  // which is not a fault — the web UI falls back to the index.
  const char *nameOf(uint8_t remote) const;

  // Writes a new configuration: persists it, then publishes it retained. Used by the web
  // UI, which is the only place configuration is edited.
  bool applyConfig(const cfg::ConfigDoc &doc, const char *writer);

  const cfg::ConfigDoc &config() const { return _config; }

  // Tells Home Assistant to forget a remote's three entities. An empty retained payload on
  // a discovery topic is how MQTT discovery expresses deletion.
  void publishDiscoveryRemoval(uint8_t remote);

  // Held here because this is where the retained topics carrying it arrive, and because
  // acting on a heard press means publishing cover state.
  const ctl::ControlMap &controls() const { return _controls; }

  // False means the broker did not take it — worth surfacing, since the retained topic is
  // the only durable copy and somebody walked a house to produce it.
  bool saveControl(const ctl::Control &control);
  bool forgetControl(uint32_t address);

 private:
  bool connect();
  void finishReconcile();
  void reconcileCounters();
  void reconcileConfig();
  void loadConfigFromStore();
  void publishDiscovery(uint8_t remote);
  void publishState(uint8_t remote);
  void publishCounter(uint8_t remote, bool force = false);
  void publishBridgeDiscovery();
  bool publishControlDiscovery(const ctl::Control &control);
  bool republishControl(const ctl::Control &control);
  void publishControlRemoval(uint32_t address);
  void applyHeardPresses();
  void publishHealth();
  void publishConfigDocument();
  void onMessage(const char *topic, const uint8_t *payload, unsigned int length);

  WiFiClient _wifi;
  PubSubClient _mqtt;
  Remotes &_remotes;
  Store &_store;
  Receiver &_receiver;
  const char *_clientId;

  ctl::ControlMap _controls;

  cfg::ConfigDoc _config;          // what the device believes
  cfg::ConfigDoc _staged;          // what arrived retained, awaiting reconciliation
  bool _haveConfig = false;
  bool _haveStaged = false;

  // The mirror as received, and the highest value this device has either published or
  // seen. The second is what enforces the floor: a publish that would lower it is
  // suppressed rather than sent.
  uint32_t _mirror[rs::MAX_REMOTES] = {0};
  bool _haveMirror[rs::MAX_REMOTES] = {false};
  uint32_t _mirrorSeen[rs::MAX_REMOTES] = {0};

  char _names[rs::MAX_REMOTES][24] = {};

  uint32_t _publishedVersion[rs::MAX_REMOTES] = {0};
  uint32_t _lastAttempt = 0;
  uint32_t _retryMs = 5000;
  uint32_t _reconcileStart = 0;
  uint32_t _lastHealth = 0;
  bool _attempted = false;
  bool _reconciling = false;
  bool _loaded = false;
};
