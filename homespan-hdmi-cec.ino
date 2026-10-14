#include "HomeSpan.h"
#include "CEC_Device.h"

#define CEC_GPIO 3              // ESPC5-12: GPIO3 sits on the ESP-12F IO14 pad (pin 5), the SLWF-08's CEC line
#define CEC_DEVICE_TYPE CEC_Device::CDT_PLAYBACK_DEVICE
#define CEC_PHYSICAL_ADDRESS 0x1000
#define CEC_TIMER_US 50         // how often the timer interrupt runs the CEC state machine

class HomeSpanTV;

// A CEC frame passed between the timer interrupt and the HomeSpan task
struct CECFrame {
  enum Event { READY, RECEIVED, SENT } event;
  int address;                  // outgoing: target logical address, READY: our logical address
  int count;
  bool ack;
  unsigned char buffer[16];
};

QueueHandle_t cecTxQueue;       // frames waiting to be transmitted
QueueHandle_t cecRxQueue;       // events from the interrupt, handled in HomeSpanTV::loop()

// implement application specific CEC device
class MyCEC_Device : public CEC_Device
{
  HomeSpanTV* connectedTV;
  public:
    void SetTVDevice(HomeSpanTV* tv);
    void Send(int targetAddress, const unsigned char* buffer, int count);
    void OnTimer();
    void HandleEvents();
  protected:
    virtual bool LineState();
    virtual void SetLineState(bool);
    virtual void OnReady(int logicalAddress);
    virtual void OnReceiveComplete(unsigned char* buffer, int count, bool ack);
    virtual void OnTransmitComplete(unsigned char* buffer, int count, bool ack);
  private:
    void QueueEvent(CECFrame::Event event, int address, unsigned char* buffer, int count, bool ack);
    void HandleFrame(unsigned char* buffer, int count);
};

bool MyCEC_Device::LineState()
{
  return gpio_get_level((gpio_num_t)CEC_GPIO);
}

void MyCEC_Device::SetLineState(bool state)
{
  // The pin is open-drain: 0 pulls the line low, 1 releases it to the pull-up.
  // CEC_Device itself waits for the line to settle before sampling it again.
  gpio_set_level((gpio_num_t)CEC_GPIO, state);
}

// --- TIMER INTERRUPT ---
// Runs every CEC_TIMER_US so the bit timing holds while Wi-Fi and HomeKit keep the CPU busy.
// All CEC_Device work happens here, including the callbacks below.
void MyCEC_Device::OnTimer()
{
  Run();

  // CEC_Device holds one outgoing frame at a time, so hand it the next one once it's free
  CECFrame frame;
  if (xQueuePeekFromISR(cecTxQueue, &frame) && TransmitFrame(frame.address, frame.buffer, frame.count))
    xQueueReceiveFromISR(cecTxQueue, &frame, NULL);
}

// The callbacks run inside the interrupt: only queue the event, HandleEvents() deals with it
void MyCEC_Device::QueueEvent(CECFrame::Event event, int address, unsigned char* buffer, int count, bool ack)
{
  CECFrame frame = {event, address, count, ack};
  if (count > 0) memcpy(frame.buffer, buffer, count);
  xQueueSendFromISR(cecRxQueue, &frame, NULL);
}

void MyCEC_Device::OnReady(int logicalAddress) { QueueEvent(CECFrame::READY, logicalAddress, NULL, 0, true); }
void MyCEC_Device::OnReceiveComplete(unsigned char* buffer, int count, bool ack) { QueueEvent(CECFrame::RECEIVED, 0, buffer, count, ack); }
void MyCEC_Device::OnTransmitComplete(unsigned char* buffer, int count, bool ack) { QueueEvent(CECFrame::SENT, 0, buffer, count, ack); }

// Queue a frame for the interrupt to transmit (frames go out one after another, in order)
void MyCEC_Device::Send(int targetAddress, const unsigned char* buffer, int count)
{
  CECFrame frame = {};
  frame.address = targetAddress;
  frame.count = count;
  memcpy(frame.buffer, buffer, count);
  if (xQueueSend(cecTxQueue, &frame, 0) != pdTRUE)
    Serial.printf("CEC transmit queue full, frame dropped\n");
}

MyCEC_Device device;

void onCECTimer() { device.OnTimer(); }

struct HomeSpanTVSpeaker : Service::TelevisionSpeaker {
    SpanCharacteristic *volume = new Characteristic::VolumeSelector();
    SpanCharacteristic *volumeType = new Characteristic::VolumeControlType(3);

    HomeSpanTVSpeaker(const char *name) : Service::TelevisionSpeaker() {
      new Characteristic::ConfiguredName(name);
      Serial.printf("Configured Speaker: %s\n", name);
    }

    boolean update() override {
      if (volume->updated()) {
        // <User Control Pressed> Volume Up (0x41) / Volume Down (0x42), then <User Control Released>
        if (volume->getNewVal() == 0) {
          Serial.printf("Volume Up\n");
          device.Send(0x0, (unsigned char*)"\x44\x41", 2);
        } else {
          Serial.printf("Volume Down\n");
          device.Send(0x0, (unsigned char*)"\x44\x42", 2);
        }
        device.Send(0x0, (unsigned char*)"\x45", 1);
      }

      if (volumeType->updated()) {
        Serial.printf("Updated volume type\n");
      }

      return (true);
    }
};

struct HomeSpanTV : Service::Television {

  SpanCharacteristic *active = new Characteristic::Active(0);                     
  SpanCharacteristic *activeID = new Characteristic::ActiveIdentifier(3);         
  SpanCharacteristic *remoteKey = new Characteristic::RemoteKey();                
  SpanCharacteristic *settingsKey = new Characteristic::PowerModeSelection();     

  // NEW: Command Queue Variables
  int pendingInput = 0;    
  uint32_t lastInteractionTime = 0;

  HomeSpanTV(const char *name) : Service::Television() {
    new Characteristic::ConfiguredName(name);             
  }

  boolean update() override {

    // --- 1. POWER CONTROL ---
    if (active->updated()) {
      Serial.printf("Set TV Power to: %s\n", active->getNewVal() ? "ON" : "OFF");
      if (active->getNewVal()) device.Send(0x0, (unsigned char*)"\x04", 1);
      else device.Send(0x0, (unsigned char*)"\x36", 1);
      lastInteractionTime = millis(); // Reset bus timer
    }

    // --- 2. INPUT SWITCHING (Queue it up!) ---
    if (activeID->updated()) {
      pendingInput = activeID->getNewVal();
      Serial.printf("HomeKit requests HDMI %d. Queuing to prevent bus crash...\n", pendingInput);
      lastInteractionTime = millis(); // Resets the 800ms countdown every time you tap
    }

    // --- 3. REMOTE CONTROL WIDGET ---
    if (remoteKey->updated()) {
      unsigned char pressCmd[2] = {0x44, 0x00};
      bool validKey = true;
      switch (remoteKey->getNewVal()) {
        case 4: pressCmd[1] = 0x01; break; case 5: pressCmd[1] = 0x02; break; 
        case 6: pressCmd[1] = 0x03; break; case 7: pressCmd[1] = 0x04; break; 
        case 8: pressCmd[1] = 0x00; break; case 9: pressCmd[1] = 0x0D; break; 
        case 11: pressCmd[1]= 0x44; break; case 15: pressCmd[1]= 0x35; break; 
        default: validKey = false;
      }
      if (validKey) {
        device.Send(0x0, pressCmd, 2);
        device.Send(0x0, (unsigned char*)"\x45", 1);
        lastInteractionTime = millis(); // Reset bus timer
      }
    }

    return (true);
  }

  // --- BACKGROUND QUEUE EXECUTOR ---
  void loop() override {
    device.HandleEvents();

    // If there is an input waiting AND you haven't pressed anything for 800ms
    if (pendingInput > 0 && (millis() - lastInteractionTime > 800)) {
      
      Serial.printf("Executing clean switch to HDMI %d\n", pendingInput);
      
      if (pendingInput == 1) {
        device.Send(0x0F, (unsigned char*)"\x82\x10\x00", 3);
        device.Send(0x0F, (unsigned char*)"\x86\x10\x00", 3);
      } else if (pendingInput == 2) {
        device.Send(0x0F, (unsigned char*)"\x82\x20\x00", 3);
        device.Send(0x0F, (unsigned char*)"\x86\x20\x00", 3);
      } else if (pendingInput == 3) {
        device.Send(0x0F, (unsigned char*)"\x82\x30\x00", 3);
        device.Send(0x0F, (unsigned char*)"\x86\x30\x00", 3);
      }
      
      pendingInput = 0; // Clear the queue!
      lastInteractionTime = millis();
    }
  }
};
void MyCEC_Device::SetTVDevice(HomeSpanTV* tv) {
  connectedTV = tv;
  //tv::active->setVal(true);
};

// Runs in HomeSpanTV::loop(), so HomeSpan values can be changed safely here
void MyCEC_Device::HandleEvents()
{
  CECFrame frame;
  while (xQueueReceive(cecRxQueue, &frame, 0)) {
    if (frame.event == CECFrame::READY) {
      // The logical address has been allocated
      unsigned char buf[4] = {0x84, CEC_PHYSICAL_ADDRESS >> 8, CEC_PHYSICAL_ADDRESS & 0xff, CEC_DEVICE_TYPE};
      DbgPrint("Device ready, Logical address assigned: %d\n", frame.address);
      Send(0xf, buf, 4); // <Report Physical Address>
    } else if (frame.event == CECFrame::SENT) {
      DbgPrint("Packet sent at %ld: %02X", millis(), frame.buffer[0]);
      for (int i = 1; i < frame.count; i++)
        DbgPrint(":%02X", frame.buffer[i]);
      if (!frame.ack)
        DbgPrint(" NAK");
      DbgPrint("\n");
    } else {
      HandleFrame(frame.buffer, frame.count);
    }
  }
}

void MyCEC_Device::HandleFrame(unsigned char* buffer, int count)
{
  if (count < 2) return; // header only (<Polling Message>)

  // 1. Physical Remote: TV turned off
  if (buffer[0] == 0x0F && buffer[1] == 0x36) {
    if (connectedTV->active->getVal() != 0) {
      connectedTV->active->setVal(0); 
      Serial.println("Sync: TV turned OFF via remote");
    }
  }

  // 2. Physical Remote: Input changed
  int physicalAddress = -1;
  if (buffer[1] == 0x82 && count >= 4) physicalAddress = (buffer[2] << 8) | buffer[3];
  else if (buffer[1] == 0x80 && count >= 6) physicalAddress = (buffer[4] << 8) | buffer[5];
  else if (buffer[1] == 0x86 && count >= 4) physicalAddress = (buffer[2] << 8) | buffer[3];

  if (physicalAddress != -1) {
    int reportedInput = 0;
    if (physicalAddress == 0x1000) reportedInput = 1;
    else if (physicalAddress == 0x2000) reportedInput = 2;
    else if (physicalAddress == 0x3000) reportedInput = 3;

    // Only sync if the change didn't come from our own HomeKit queue
    if (reportedInput > 0 && connectedTV->pendingInput == 0 && connectedTV->activeID->getVal() != reportedInput) {
      connectedTV->activeID->setVal(reportedInput); 
      if (connectedTV->active->getVal() == 0) connectedTV->active->setVal(1);
      Serial.printf("Sync: TV updated to HDMI %d\n", reportedInput);
    }
  }

  if ((buffer[0] & 0xf) != LogicalAddress() && (buffer[0] & 0xf) != 0xF) return; 

  switch (buffer[1]) {
    case 0x83: { 
        unsigned char buf[4] = {0x84, CEC_PHYSICAL_ADDRESS >> 8, CEC_PHYSICAL_ADDRESS & 0xff, CEC_DEVICE_TYPE};
        Send(0xf, buf, 4);
        break;
      }
    case 0x8c: Send(0xf, (unsigned char*)"\x87\x01\x23\x45", 4); break;
    case 0x46: Send(buffer[0] >> 4, (unsigned char*)"\x47\x52\x44\x4d", 4); break; // <Set OSD Name> goes back to whoever asked
  }
}

///////////////////////////////

void setup() {
  gpio_reset_pin((gpio_num_t)CEC_GPIO);

  Serial.begin(115200);

  cecTxQueue = xQueueCreate(16, sizeof(CECFrame));
  cecRxQueue = xQueueCreate(16, sizeof(CECFrame));

  WiFi.STA.begin();                               // 5 GHz only (ESP32-C5)
  WiFi.setBandMode(WIFI_BAND_MODE_5G_ONLY);

  homeSpan.enableOTA();
  homeSpan.begin(Category::Television, "HomeSpan Television");

  SPAN_ACCESSORY();

  // Below we define 10 different InputSource Services using different combinations
  // of Characteristics to demonstrate how they interact and appear to the user in the Home App

// --- HDMI 1 ---
  SpanService *hdmi1 = new Service::InputSource();    
  new Characteristic::ConfiguredName("HDMI 1");
  new Characteristic::InputSourceType(3);           // 3 = HDMI Type (Required by iOS!)
  new Characteristic::IsConfigured(1);              // 1 = Configured
  new Characteristic::CurrentVisibilityState(0);    // 0 = Shown in the selection menu
  new Characteristic::Identifier(1);

  // --- HDMI 2 ---
  SpanService *hdmi2 = new Service::InputSource();
  new Characteristic::ConfiguredName("HDMI 2");
  new Characteristic::InputSourceType(3);
  new Characteristic::IsConfigured(1);              
  new Characteristic::CurrentVisibilityState(0);    
  new Characteristic::Identifier(2);

  // --- HDMI 3 ---
  SpanService *hdmi3 = new Service::InputSource();
  new Characteristic::ConfiguredName("HDMI 3");
  new Characteristic::InputSourceType(3);
  new Characteristic::IsConfigured(1);              
  new Characteristic::CurrentVisibilityState(0);    
  new Characteristic::Identifier(3);

  // --- SPEAKER ---
  // Note: I removed the extra VolumeSelector() and VolumeControlType() that were here, 
  // because they are already created inside the HomeSpanTVSpeaker struct!
  SpanService *speaker = new HomeSpanTVSpeaker("My Speaker");

  HomeSpanTV* tv = (new HomeSpanTV("Samsung TV"));                         // Define a Television Service.  Must link in InputSources!
  tv->addLink(hdmi1)
  ->addLink(hdmi2)
  ->addLink(hdmi3)
  ->addLink(speaker)
  ;

  device.SetTVDevice(tv);

  // CEC line: open-drain with pull-up, released (high) to start
  gpio_set_level((gpio_num_t)CEC_GPIO, 1);
  gpio_set_direction((gpio_num_t)CEC_GPIO, GPIO_MODE_INPUT_OUTPUT_OD);
  gpio_set_pull_mode((gpio_num_t)CEC_GPIO, GPIO_PULLUP_ONLY);
  device.Initialize(CEC_PHYSICAL_ADDRESS, CEC_DEVICE_TYPE, true); // Promiscuous mode

  hw_timer_t* cecTimer = timerBegin(1000000);                     // 1 MHz, 1 tick = 1 us
  timerAttachInterrupt(cecTimer, onCECTimer);
  timerAlarm(cecTimer, CEC_TIMER_US, true, 0);                    // repeat forever

  homeSpan.autoPoll();
}

// No loop(): HomeSpan supplies one when autoPoll() is used, and CEC runs from the timer interrupt