#include <Arduino.h>
#include <Wire.h>
#include <PN532_I2C.h>
#include <PN532.h>
#include <NfcAdapter.h>
#include <BLEDevice.h>
#include <BLEHIDDevice.h>
#include <HIDTypes.h>
#include <HIDKeyboardTypes.h>

// defines
#define SERIAL_DEBUG 1
#define US_KEYBOARD 1
#define DEVICE_NAME "Bluetooth RFID Reader"
#define MANUFACTURER_NAME "BRS"
#define NFC_DEBOUNCE_MICRO_SECONDS 1000 * 200
#define HID_INPUT_SUFFIX "\r\n"

// function headers
void bluetooth_setup(void *);
void hid_type_text(const char *text);
void nfc_setup();
bool nfc_read();

// variables
PN532_I2C pn532_i2c(Wire);
PN532 nfc(pn532_i2c);
String nfc_tag_uid = "";
char byte_to_decimal_buffer[5];
int64_t last_read_time = 0;

bool bt_client_connected = false;
TaskHandle_t bt_task_handle = NULL;
BLEHIDDevice *hid;
BLECharacteristic *input;
BLECharacteristic *output;

void setup()
{
    Serial.begin(115200);
    while (!Serial)
        delay(10);

    nfc_setup();
    xTaskCreate(bluetooth_setup, "bluetooth", 20000, NULL, 5, &bt_task_handle);
}

void loop(void)
{
    bool nfc_tag_read = nfc_read();

    if (nfc_tag_read)
    {
        // debounce repeating reads of the nfc tag
        if (esp_timer_get_time() - last_read_time < NFC_DEBOUNCE_MICRO_SECONDS)
        {
            // skip read, because card has been hovering over reader.
            Serial.println("skipped ");
        }
        else
        {
            Serial.print("Card scanned: ");
            Serial.println(nfc_tag_uid);
            if (bt_client_connected)
            {
                Serial.println("Send to HID input");
                nfc_tag_uid += HID_INPUT_SUFFIX;
                hid_type_text(nfc_tag_uid.c_str());
            }
        }
        last_read_time = esp_timer_get_time();
        nfc_tag_uid = "";
    }

    delay(100);
}

void nfc_setup()
{
    nfc.begin();

    uint32_t version_data = nfc.getFirmwareVersion();
    if (!version_data)
    {
        Serial.print("Can not find PN5XX board over i2c, restart");
        // Restart ESP in order to prevent i2c hiccup
        ESP.restart();
    }

    // Print Information
    Serial.print("Found chip: PN5");
    Serial.println((version_data >> 24) & 0xFF, HEX);
    Serial.print("Firmware version: ");
    Serial.print((version_data >> 16) & 0xFF, DEC);
    Serial.print('.');
    Serial.println((version_data >> 8) & 0xFF, DEC);

    // Set Timeout tries for passive reading
    nfc.setPassiveActivationRetries(0xFE);

    // configure board to read with Secure Access Module
    nfc.SAMConfig();

    Serial.println("Waiting for ISO14443A card");
}

bool nfc_read()
{
    boolean card_detected;
    // uid buffer
    uint8_t uid[] = {0, 0, 0, 0, 0, 0, 0};
    // UID length (4 or 7 bytes depending on card type)
    uint8_t uidLength;
    card_detected = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, &uid[0], &uidLength);

    // If the card is detected
    if (card_detected)
    {
        // read uid into nfc_tag_uid as Decimal
        for (uint8_t i = 0; i < uidLength; i++)
        {
            sprintf(byte_to_decimal_buffer, "%d", uid[i]);
            nfc_tag_uid += byte_to_decimal_buffer;
        }
        return true;
    }

    return false;
}

// Message (report) sent when a key is pressed or released
struct InputValue
{
    uint8_t modifiers;      // bitmask: CTRL = 1, SHIFT = 2, ALT = 4
    uint8_t reserved;       // must be 0
    uint8_t pressedKeys[6]; // up to six concurrently pressed keys
};

// Message (report) received when an LED's state changed
struct OutputValue
{
    uint8_t leds; // bitmask: num lock = 1, caps lock = 2, scroll lock = 4, compose = 8, kana = 16
};

// The report map describes the HID device (a keyboard in this case) and
// the messages (reports in HID terms) sent and received.
static const uint8_t REPORT_MAP[] = {
    USAGE_PAGE(1), 0x01, // Generic Desktop Controls
    USAGE(1), 0x06,      // Keyboard
    COLLECTION(1), 0x01, // Application

    REPORT_ID(1), 0x01,       //   Report ID (1)
    USAGE_PAGE(1), 0x07,      //   Keyboard/Keypad
    USAGE_MINIMUM(1), 0xE0,   //   Keyboard Left Control
    USAGE_MAXIMUM(1), 0xE7,   //   Keyboard Right Control
    LOGICAL_MINIMUM(1), 0x00, //   Each bit is either 0 or 1
    LOGICAL_MAXIMUM(1), 0x01,
    REPORT_COUNT(1), 0x08, //   8 bits -> InputValue.modifiers
    REPORT_SIZE(1), 0x01,
    HIDINPUT(1), 0x02, //   Data, Var, Abs

    REPORT_COUNT(1), 0x01, //   1 byte (unused) -> InputValue.reserved
    REPORT_SIZE(1), 0x08,
    HIDINPUT(1), 0x01, //   Const, Array, Abs

    REPORT_COUNT(1), 0x06, //   6 bytes -> InputValue.pressedKeys
    REPORT_SIZE(1), 0x08,
    LOGICAL_MINIMUM(1), 0x00,
    LOGICAL_MAXIMUM(1), 0x65, //   101  supported keys
    USAGE_MINIMUM(1), 0x00,
    USAGE_MAXIMUM(1), 0x65,
    HIDINPUT(1), 0x00, //   Data, Array, Abs

    REPORT_COUNT(1), 0x05, //   5 bits (Num lock, Caps lock, Scroll lock, Compose, Kana) -> OutputValue.leds
    REPORT_SIZE(1), 0x01,
    USAGE_PAGE(1), 0x08,    //   LEDs
    USAGE_MINIMUM(1), 0x01, //   Num Lock
    USAGE_MAXIMUM(1), 0x05, //   Kana
    LOGICAL_MINIMUM(1), 0x00,
    LOGICAL_MAXIMUM(1), 0x01,
    HIDOUTPUT(1), 0x02, //   Data, Var, Abs

    REPORT_COUNT(1), 0x01, //   3 bits (Padding) -> OutputValue.leds
    REPORT_SIZE(1), 0x03,
    HIDOUTPUT(1), 0x01, //   Const, Array, Abs

    END_COLLECTION(0) // End application collection
};

const InputValue NO_KEY_PRESSED = {};

/*
 * Callbacks related to BLE connection
 */
class BleKeyboardCallbacks : public BLEServerCallbacks
{

    void onConnect(BLEServer *server)
    {
        bt_client_connected = true;

        // Allow notifications for characteristics
        BLE2902 *cccDesc = (BLE2902 *)input->getDescriptorByUUID(BLEUUID((uint16_t)0x2902));
        cccDesc->setNotifications(true);

        Serial.println("Client has connected");
    }

    void onDisconnect(BLEServer *server)
    {
        bt_client_connected = false;

        // Disallow notifications for characteristics
        BLE2902 *cccDesc = (BLE2902 *)input->getDescriptorByUUID(BLEUUID((uint16_t)0x2902));
        cccDesc->setNotifications(false);

        Serial.println("Client has disconnected");
    }
};

/*
 * Called when the client (computer, smart phone) wants to turn on or off
 * the LEDs in the keyboard.
 *
 * bit 0 - NUM LOCK
 * bit 1 - CAPS LOCK
 * bit 2 - SCROLL LOCK
 */
class OutputCallbacks : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *characteristic)
    {
        OutputValue *report = (OutputValue *)characteristic->getData();
        // read output, value not needed.
    }
};

void bluetooth_setup(void *)
{

    // initialize the device
    BLEDevice::init(DEVICE_NAME);
    BLEServer *server = BLEDevice::createServer();
    server->setCallbacks(new BleKeyboardCallbacks());

    // create an HID device
    hid = new BLEHIDDevice(server);
    input = hid->inputReport(1);   // report ID
    output = hid->outputReport(1); // report ID
    output->setCallbacks(new OutputCallbacks());

    // set manufacturer name
    hid->manufacturer()->setValue(MANUFACTURER_NAME);
    // set usb Plug and Play (pnp) infos
    // sId , vendorId, productId , versionId
    hid->pnp(0x02, 0xe502, 0xa111, 0x0210);
    // information about HID device: device is not localized, device can be connected
    hid->hidInfo(0x00, 0x02);

    // Set Security of device to only bonding
    BLESecurity *security = new BLESecurity();
    security->setAuthenticationMode(ESP_LE_AUTH_BOND);

    // Defines Data Structure that is send via the report map
    hid->reportMap((uint8_t *)REPORT_MAP, sizeof(REPORT_MAP));
    hid->startServices();

    hid->setBatteryLevel(100);

    // advertise the services
    BLEAdvertising *advertising = server->getAdvertising();
    advertising->setAppearance(HID_KEYBOARD);
    advertising->addServiceUUID(hid->hidService()->getUUID());
    advertising->addServiceUUID(hid->deviceInfo()->getUUID());
    advertising->addServiceUUID(hid->batteryService()->getUUID());
    advertising->start();

    Serial.println("Bluetooth is setup and waiting for connection");
    delay(portMAX_DELAY);
};

void hid_type_text(const char *text)
{
    int len = strlen(text);
    for (int i = 0; i < len; i++)
    {
        uint8_t val = (uint8_t)text[i];
        // check if char is available on keyboard
        if (val > KEYMAP_SIZE)
        {
            continue;
        }
        KEYMAP keymap_entry = keymap[val];

        InputValue input_value = {
            .modifiers = keymap_entry.modifier,
            .reserved = 0,
            .pressedKeys = {
                keymap_entry.usage,
                0, 0, 0, 0, 0}};

        // send the input report
        input->setValue((uint8_t *)&input_value, sizeof(input_value));
        input->notify();
        delay(5);

        // release all keys between two characters; otherwise two identical
        // consecutive characters are treated as just one key press
        input->setValue((uint8_t *)&NO_KEY_PRESSED, sizeof(NO_KEY_PRESSED));
        input->notify();
        delay(5);
    }
}
