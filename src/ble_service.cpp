#include "app.h"

bool deviceConnected = false;
NimBLECharacteristic* pTxChar = nullptr;

bool          tindeqMeasuring    = false;
bool          tindeqTareReq      = false;
unsigned long measureStartMicros = 0;

// Tindeq data characteristic — internal; only the notify helpers touch it.
static NimBLECharacteristic* pTindeqData = nullptr;

// ─── BLE callbacks ───────────────────────────────────────────────────────────
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    deviceConnected = true;
    Serial.println("[BLE] connected");
  }
  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    deviceConnected = false;
    Serial.println("[BLE] disconnected — re-advertising");
    NimBLEDevice::startAdvertising();
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
    std::string val = pChar->getValue();
    if (val.empty()) return;
    String reply = "Echo: " + String(val.c_str());
    pTxChar->setValue(reply.c_str());
    pTxChar->notify();
  }
};

// ─── Tindeq Progressor notifications ─────────────────────────────────────────
// Packet layout: [response_code][payload_len][payload...]
// A weight sample payload is one 8-byte pair: float32 kg + uint32 timestamp_us.
void tindeqNotifyWeight(float kg, uint32_t tsMicros) {
  if (!pTindeqData) return;
  uint8_t pkt[10];
  pkt[0] = TINDEQ_RES_WEIGHT;
  pkt[1] = 8;                       // one sample
  memcpy(&pkt[2], &kg, 4);          // float32, little-endian
  memcpy(&pkt[6], &tsMicros, 4);    // uint32,  little-endian
  pTindeqData->setValue(pkt, sizeof(pkt));
  pTindeqData->notify();
}

// Command response (e.g. battery voltage, version string)
static void tindeqNotifyCmdResponse(const uint8_t* payload, uint8_t len) {
  if (!pTindeqData) return;
  uint8_t pkt[24];
  if (len > sizeof(pkt) - 2) len = sizeof(pkt) - 2;
  pkt[0] = TINDEQ_RES_CMD;
  pkt[1] = len;
  memcpy(&pkt[2], payload, len);
  pTindeqData->setValue(pkt, len + 2);
  pTindeqData->notify();
}

// Control-point writes from the Tindeq app. Keep this non-blocking: heavy work
// (tare averaging) is deferred to loop() via a flag.
class TindeqCtrlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
    std::string v = pChar->getValue();
    if (v.empty()) return;
    uint8_t op = (uint8_t)v[0];     // opcode is the first byte (TLV; args ignored)
    switch (op) {
      case TINDEQ_CMD_TARE:
        tindeqTareReq = true;
        Serial.println("[TQ] tare");
        break;
      case TINDEQ_CMD_START_MEAS:
        measureStartMicros = micros();
        tindeqMeasuring = true;
        Serial.println("[TQ] start measurement");
        break;
      case TINDEQ_CMD_STOP_MEAS:
        tindeqMeasuring = false;
        Serial.println("[TQ] stop measurement");
        break;
      case TINDEQ_CMD_GET_BATT: {
        uint32_t mv = batteryMv;   // real measured battery voltage
        tindeqNotifyCmdResponse((uint8_t*)&mv, 4);
        Serial.println("[TQ] battery query");
        break;
      }
      case TINDEQ_CMD_GET_APP_VER: {
        const char* ver = "1.0.0";
        tindeqNotifyCmdResponse((const uint8_t*)ver, strlen(ver));
        Serial.println("[TQ] version query");
        break;
      }
      case TINDEQ_CMD_ENTER_SLEEP:
        tindeqMeasuring = false;
        Serial.println("[TQ] sleep (ignored)");
        break;
      default:
        Serial.printf("[TQ] unhandled opcode %u\n", op);
        break;
    }
  }
};

// ─── Setup ───────────────────────────────────────────────────────────────────
void bleSetup() {
  // Advertise as a Tindeq Progressor ("Progressor_XXXX") so the Tindeq app
  // discovers it. XXXX is derived from the chip MAC for a stable unique name.
  char devName[20];
  uint16_t devId = (uint16_t)(ESP.getEfuseMac() & 0xFFFF);
  snprintf(devName, sizeof(devName), "Progressor_%04X", devId);
  NimBLEDevice::init(devName);
  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  NimBLEService* pService = pServer->createService(NUS_SERVICE_UUID);
  pTxChar = pService->createCharacteristic(NUS_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic* pRxChar = pService->createCharacteristic(
    NUS_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pRxChar->setCallbacks(new RxCallbacks());
  pService->start();

  // Tindeq Progressor service (runs in parallel with NUS)
  NimBLEService* pTindeqSvc = pServer->createService(TINDEQ_SERVICE_UUID);
  pTindeqData = pTindeqSvc->createCharacteristic(TINDEQ_DATA_UUID, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic* pTindeqCtrl = pTindeqSvc->createCharacteristic(
    TINDEQ_CTRL_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pTindeqCtrl->setCallbacks(new TindeqCtrlCallbacks());
  pTindeqSvc->start();

  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  // Advertise the Tindeq service UUID so the app filters us in (one 128-bit
  // UUID fills the adv packet; the name rides in the scan response).
  pAdv->addServiceUUID(TINDEQ_SERVICE_UUID);
  pAdv->enableScanResponse(true);
  pAdv->setName(devName);
  pAdv->setMinInterval(0x20);
  pAdv->setMaxInterval(0x40);
  pAdv->start();
  Serial.printf("[BOOT] advertising as '%s' (Tindeq Progressor + NUS)\n", devName);
}
