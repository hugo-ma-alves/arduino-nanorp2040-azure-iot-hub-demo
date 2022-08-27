#include "az_result.h"
#include "az_json.h"
#include <Arduino.h>

//Wifi libraries
#include <SPI.h>
#include <WiFiNINA.h>

// Libraries for MQTT client, WiFi connection and SAS-token generation.
#include <PubSubClient.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>
#include "mbedtls/md.h"

// Azure IoT SDK for C includes
#include <az_core.h>
#include <az_iot.h>

#include "telemetry.h"
#include "secrets.h"

#define sizeofarray(a) (sizeof(a) / sizeof(a[0]))
#define ONE_HOUR_IN_SECS 3600
#define MQTT_PACKET_SIZE 512
#define MQTT_CLIENT_ID_BUFFER_SIZE 256
#define MQTT_USERNAME_BUFFER_SIZE 350
#define PLAIN_SAS_SIGNATURE_BUFFER_SIZE 256
#define MQTT_PASSWORD_BUFFER_SIZE 512  //SAS Ttoken max size - mqtt password max size
#define SAS_HMAC256_ENCRYPTED_SIGNATURE_BUFFER_SIZE 32
#define DECODED_SAS_KEY_BUFFER_SIZE 32

#define TELEMETRY_PROPERTY_TEMPERATURE "temperature"
#define TELEMETRY_PROPERTY_DEVICE_NAME "deviceId"
#define DEVICE_NAME "arduino_nano_rp2040"

// When developing for your own Arduino-based platform,
// please follow the format '(ard;<platform>)'.
#define AZURE_SDK_CLIENT_USER_AGENT "c%2F" AZ_SDK_VERSION_STRING "(ard;rp2040)"
//Max payload size of the message sent to azure hub
#define MAX_TELEMETRY_PAYLOAD_SIZE 256

static WiFiSSLClient wifi_client;

static PubSubClient mqtt_client(wifi_client);
static az_iot_hub_client iothub_client;

//Sas token vars
static char mqtt_password[MQTT_PASSWORD_BUFFER_SIZE];
static uint64_t expiration_time;

//Azure Iot connection properties
static const int port = AZ_IOT_DEFAULT_MQTT_CONNECT_PORT;  //8883
static const char *host = IOT_CONFIG_IOTHUB_FQDN;
static const char *device_id = IOT_CONFIG_DEVICE_ID;
//Topic name is obtained by azure sdk
static char telemetry_topic[128];

static void initialize_mqtt_client();
static void establish_connection();
static void generate_mqtt_password();
static void connect_to_azure_iot_hub();
static void build_telemetry_payload(float temperature, char *out_payload_buffer, size_t payload_buffer_size, size_t *payload_buffer_length);
static void stop_if_fail(az_result rc, char const *message);
static uint64_t get_time();

/**
* Uploads the specified metric to Azure iot hub.
* It sends a json payload with the format {"deviceId":"arduino", "temperature":25.61,"OtherMetric":10}
*/
void upload_telemetry(float temperature) {
  // if not connected connect and send metric
  // Check if connected, reconnect if needed.
  if (!mqtt_client.connected()) {
    establish_connection();
  } else if (expiration_time < get_time() || (expiration_time - get_time()) < 30) {  //renew if sas is about to expire
    Serial.println("Renewing SAS token");
    mqtt_client.disconnect();
    establish_connection();
  }

  digitalWrite(LEDG, HIGH);
  char payloadBuffer[MAX_TELEMETRY_PAYLOAD_SIZE];
  size_t payload_size;
  build_telemetry_payload(temperature, payloadBuffer, MAX_TELEMETRY_PAYLOAD_SIZE, &payload_size);
  //Serial.println(payloadBuffer);

  boolean success = mqtt_client.publish(telemetry_topic, (char *)payloadBuffer, false);
  digitalWrite(LEDG, LOW);
  if (success) {
    Serial.println("Metric sent to Iot hub");
  } else {
    digitalWrite(LEDR, HIGH);
    delay(1000);
    digitalWrite(LEDR, LOW);
    Serial.print("Failed to send metric with the state ");
    Serial.println(mqtt_client.state());
  }
}

void telemetry_poll() {
  // This should be called regularly to allow the client to process incoming messages and maintain its connection to the server.
  mqtt_client.loop();
}

static uint64_t get_time() {
  return (uint64_t)WiFi.getTime();
}

/**
* Estblishes wifi and the mqtt connection
*/
static void establish_connection() {
  digitalWrite(LEDB, HIGH);
  //Initialize azure sdk client and the mqtt client
  initialize_mqtt_client();
  //Generate the SAS token (mqtt password)
  generate_mqtt_password();
  //Connects to azure using the mqtt protocol
  connect_to_azure_iot_hub();
  digitalWrite(LEDB, LOW);
}

static void connect_to_azure_iot_hub() {
  size_t client_id_length;
  char mqtt_client_id[MQTT_CLIENT_ID_BUFFER_SIZE];
  az_result rc;

  rc = az_iot_hub_client_get_client_id(&iothub_client, mqtt_client_id, sizeof(mqtt_client_id) - 1, &client_id_length);
  stop_if_fail(rc, "[ERROR] Failed getting client id");
  mqtt_client_id[client_id_length] = '\0';

  char mqtt_username[MQTT_USERNAME_BUFFER_SIZE];
  // Get the MQTT user name used to connect to IoT Hub
  rc = az_iot_hub_client_get_user_name(&iothub_client, mqtt_username, sizeofarray(mqtt_username), NULL);
  stop_if_fail(rc, "[ERROR] Failed to get MQTT clientId");

  Serial.print("Client ID: ");
  Serial.println(mqtt_client_id);

  Serial.print("Username: ");
  Serial.println(mqtt_username);

  while (!mqtt_client.connected()) {
    Serial.print("MQTT connecting ... ");
    if (mqtt_client.connect(mqtt_client_id, mqtt_username, mqtt_password)) {
      Serial.println("connected.");
    } else {
      Serial.print("[ERROR] failed, status code =");
      Serial.print(mqtt_client.state());
      Serial.println(". Trying again in 5 seconds.");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }

  //Cloud to device topic, another tutorial
  //mqtt_client.subscribe(AZ_IOT_HUB_CLIENT_C2D_SUBSCRIBE_TOPIC);
  rc = az_iot_hub_client_telemetry_get_publish_topic(&iothub_client, NULL, telemetry_topic, sizeof(telemetry_topic), NULL);
  stop_if_fail(rc, "Failed az_iot_hub_client_telemetry_get_publish_topic");
}

/**
* Initializes the azure Iot client and the mqttclient
* To create the azure client it is necessary to configure a mqtt client. 
* The mqtt client is not part of the azure sdk, it is implemented by https://pubsubclient.knolleary.net/
*/
static void initialize_mqtt_client() {
  az_iot_hub_client_options options = az_iot_hub_client_options_default();
  options.user_agent = AZ_SPAN_FROM_STR(AZURE_SDK_CLIENT_USER_AGENT);

  az_result rc = az_iot_hub_client_init(&iothub_client, az_span_create((uint8_t *)host, strlen(host)), az_span_create((uint8_t *)device_id, strlen(device_id)), &options);
  stop_if_fail(rc, "Failed initializing Azure IoT Hub client");

  mqtt_client.setServer(host, port);
  mqtt_client.setBufferSize(MQTT_PACKET_SIZE);

  Serial.println("MQTT client initialized");

  //No callback for now, next tutorial
  //mqtt_client.setCallback(receivedCallback);
}

/**
* Util function to encode in base64 a byte array
*/
static void base64_encode_bytes(az_span decoded_bytes, az_span base64_encoded_bytes, az_span *out_base64_encoded_bytes) {
  size_t len;
  if (mbedtls_base64_encode(az_span_ptr(base64_encoded_bytes), (size_t)az_span_size(base64_encoded_bytes),
                            &len, az_span_ptr(decoded_bytes), (size_t)az_span_size(decoded_bytes))
      != 0) {
    stop_if_fail(AZ_ERROR_CANCELED, "[ERROR] mbedtls_base64_encode fail");
  }

  *out_base64_encoded_bytes = az_span_create(az_span_ptr(base64_encoded_bytes), (int32_t)len);
}

static void decode_base64_bytes(az_span base64_encoded_bytes, az_span decoded_bytes, az_span *out_decoded_bytes) {

  memset(az_span_ptr(decoded_bytes), 0, (size_t)az_span_size(decoded_bytes));

  size_t len;
  if (mbedtls_base64_decode(az_span_ptr(decoded_bytes), (size_t)az_span_size(decoded_bytes),
                            &len, az_span_ptr(base64_encoded_bytes), (size_t)az_span_size(base64_encoded_bytes))
      != 0) {
    stop_if_fail(AZ_ERROR_CANCELED, "[ERROR] mbedtls_base64_decode fail");
  }

  *out_decoded_bytes = az_span_create(az_span_ptr(decoded_bytes), (int32_t)len);
}

/**
* Calculates the HMAC Sha256 of the sas signature and then returns the base64 value
*/
static void hmac_sha256_shared_access_token(az_span sas_base64_encoded_key, az_span sas_signature, az_span sas_base64_encoded_signed_signature,
                                            az_span *out_sas_base64_encoded_signed_signature) {
  // Decode the sas base64 encoded key to use for HMAC signing.
  char sas_decoded_key_buffer[DECODED_SAS_KEY_BUFFER_SIZE];
  az_span sas_decoded_key = AZ_SPAN_FROM_BUFFER(sas_decoded_key_buffer);
  decode_base64_bytes(sas_base64_encoded_key, sas_decoded_key, &sas_decoded_key);

  // Calculate the HMAC-SHA256 of the signature with the decoded key.
  char sas_hmac256_signed_signature_buffer[SAS_HMAC256_ENCRYPTED_SIGNATURE_BUFFER_SIZE];
  az_span sas_hmac256_signed_signature = AZ_SPAN_FROM_BUFFER(sas_hmac256_signed_signature_buffer);

  mbedtls_md_context_t ctx;
  mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;

  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char *)az_span_ptr(sas_decoded_key), (size_t)az_span_size(sas_decoded_key));
  mbedtls_md_hmac_update(&ctx, (const unsigned char *)az_span_ptr(sas_signature), (size_t)az_span_size(sas_signature));
  mbedtls_md_hmac_finish(&ctx, az_span_ptr(sas_hmac256_signed_signature));
  mbedtls_md_free(&ctx);

  sas_hmac256_signed_signature = az_span_create(az_span_ptr(sas_hmac256_signed_signature), az_span_size(sas_hmac256_signed_signature));

  // Base64 encode the result of the HMAC signing.
  base64_encode_bytes(sas_hmac256_signed_signature, sas_base64_encoded_signed_signature, out_sas_base64_encoded_signed_signature);
}

/**
* Builds the mqtt password and stores it in the global variable mqtt_password
* The mqtt password is the SAS (shared access signature) token:
* SharedAccessSignature sig={signature-string}&se={expiry}&skn={policyName}&sr={URL-encoded-resourceURI}
* https://docs.microsoft.com/en-us/azure/iot-hub/iot-hub-mqtt-support
* https://docs.microsoft.com/en-us/azure/iot-hub/iot-hub-dev-guide-sas?tabs=csharp#sas-token-structure
*
* The steps to build the mqtt password(SAS) are:
* 1. Gets the Shared Access clear-text signature.
* 2. Signs it using HMAC-SHA256, with the Shared Access Key as the key
* 3. Base64 encode the result.
* 4. Builds the mqtt password (SAS token)
*/
static void generate_mqtt_password() {
  az_result rc;
  // Expires in 1 hour
  expiration_time = get_time() + ONE_HOUR_IN_SECS;
  uint8_t signature_buffer[PLAIN_SAS_SIGNATURE_BUFFER_SIZE];

  // 1. Get the Gets the Shared Access clear-text signature.
  az_span sas_signature = AZ_SPAN_FROM_BUFFER(signature_buffer);
  rc = az_iot_hub_client_sas_get_signature(&iothub_client, expiration_time, sas_signature, &sas_signature);
  stop_if_fail(rc, "Could not get the signature for SAS key.");

  // 2. Sign it using HMAC-SHA256 using the Shared Access Key as key
  // 3. Base64 encode the result.
  char b64enc_hmacsha256_signature[64];
  az_span sas_base64_encoded_signed_signature = AZ_SPAN_FROM_BUFFER(b64enc_hmacsha256_signature);
  hmac_sha256_shared_access_token(AZ_SPAN_FROM_STR(IOT_CONFIG_DEVICE_KEY), sas_signature, sas_base64_encoded_signed_signature, &sas_base64_encoded_signed_signature);

  // 4. Builds the mqtt password
  size_t mqtt_password_lenght;
  rc = az_iot_hub_client_sas_get_password(&iothub_client, expiration_time, sas_base64_encoded_signed_signature, AZ_SPAN_EMPTY, mqtt_password, sizeof(mqtt_password), &mqtt_password_lenght);
  stop_if_fail(rc, "Could not get the password.");

  Serial.print("Generated the following SAS (mqtt password): ");
  Serial.println(mqtt_password);
}

/**
* Creates a Json payload with the specified metric
* [out] The generated payload is stored in the payload_buffer array
* using it in the mqtt client
* [out] The payload_buffer_length contains its length
*/
static void build_telemetry_payload(float temperature, char *out_payload_buffer, size_t payload_buffer_size, size_t *out_payload_buffer_length) {
  az_json_writer jw;
  az_result rc;
  az_span payload_buffer_span = az_span_create((uint8_t *)out_payload_buffer, payload_buffer_size);
  rc = az_json_writer_init(&jw, payload_buffer_span, NULL);
  stop_if_fail(rc, "Failed to build telemetry payload");
  rc = az_json_writer_append_begin_object(&jw);
  stop_if_fail(rc, "Failed to build telemetry payload");

  rc = az_json_writer_append_property_name(&jw, AZ_SPAN_FROM_STR(TELEMETRY_PROPERTY_TEMPERATURE));
  stop_if_fail(rc, "Failed to build telemetry payload");
  rc = az_json_writer_append_double(&jw, temperature, 2);

  rc = az_json_writer_append_property_name(&jw, AZ_SPAN_FROM_STR(TELEMETRY_PROPERTY_DEVICE_NAME));
  stop_if_fail(rc, "Failed to build telemetry payload");
  rc = az_json_writer_append_string(&jw, AZ_SPAN_FROM_STR(DEVICE_NAME));

  stop_if_fail(rc, "Failed to build telemetry payload");
  rc = az_json_writer_append_end_object(&jw);
  stop_if_fail(rc, "Failed to build telemetry payload");

  payload_buffer_span = az_json_writer_get_bytes_used_in_destination(&jw);

  out_payload_buffer[az_span_size(payload_buffer_span)] = '\0';
  *out_payload_buffer_length = az_span_size(payload_buffer_span);
}

static void stop_if_fail(az_result rc, char const *message) {
  if (!az_result_failed(rc)) {
    return;
  }

  digitalWrite(LEDB, LOW);
  digitalWrite(LEDG, LOW);
  Serial.println(message);
  Serial.print("az_result return code: ");
  Serial.println(rc);
  while (1) {
    digitalWrite(LEDR, HIGH);
    delay(1500);
    digitalWrite(LEDR, LOW);
    delay(1500);
  }
}