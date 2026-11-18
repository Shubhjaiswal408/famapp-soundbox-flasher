/* ==========================================================
 * FamApp Payment Soundbox  --  ESP32 Dev Module
 * TTS via Google Translate (no API key, no account)
 *
 * Reads FamApp credit emails straight from Gmail over IMAP,
 * shows the amount on a 0.91" 128x32 OLED, and speaks it
 * through a MAX98357A I2S amplifier.
 *
 * FAST POLLING MODE: the box asks Gmail "anything above UID n?"
 * about once a second. That one command (~0.4 s) both detects a
 * new email and returns its sender and subject, so a payment is
 * spoken ~3-5 s after Gmail receives it. (Gmail's IDLE push was
 * measured at ~27 s on this box, which is why it is not used.)
 * Press the button to hear the last payment again.
 *
 * No server, no merchant account, no backend, no API key.
 *
 * BOARD SETTINGS (required):
 *   Board            : ESP32 Dev Module
 *   Partition Scheme : Huge APP (3MB No OTA/1MB SPIFFS)
 *
 * WIRING
 *   OLED SSD1306 128x32 (I2C)      MAX98357A (I2S)
 *     VCC -> 3V3                     VIN  -> 5V (or 3V3)
 *     GND -> GND                     GND  -> GND
 *     SDA -> GPIO 21                 DIN  -> GPIO 27
 *     SCL -> GPIO 22                 BCLK -> GPIO 26
 *                                    LRC  -> GPIO 25
 *                                    GAIN -> floating (9 dB)
 *
 *   BUTTON: one leg -> GPIO 32, other leg -> GND (internal pull-up)
 *           press = repeat the last payment; hold at power-on = setup
 *
 * FIRST-TIME SETUP (no code changes needed)
 *   1. Power on. With no saved settings the box starts its own
 *      Wi-Fi network:  FamApp-Soundbox   password: soundbox
 *   2. Join it from a phone. The setup page opens by itself
 *      (or browse to http://192.168.4.1).
 *   3. Enter Wi-Fi name/password, Gmail address and Gmail app
 *      password, then Save. The box restarts and connects.
 *
 *   To change settings later: hold the button while powering on.
 *   To ERASE all settings: hold the board's BOOT button for 10 s while
 *   the box is running; it restarts into setup mode.
 *   The portal also opens by itself if Wi-Fi or Gmail login fails.
 * ========================================================== */

#include <WiFi.h>
#include <ESP_Mail_Client.h>
#include <Audio.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESP_I2S.h>

// Timing instrumentation: logs how long each step of a payment takes
// and turns on the mail library's step log. Comment out for normal use.
//#define TIMING_DEBUG

#ifdef TIMING_DEBUG
uint32_t tMark = 0;
#define TSTART()    (tMark = millis())
#define TLOG(what)  do { Serial.print("[T+"); Serial.print(millis() - tMark);                          Serial.print(" ms] "); Serial.println(what); } while (0)
#else
#define TSTART()
#define TLOG(what)
#endif

// ==========================================================
// SETTINGS -- entered through the setup portal and stored in flash.
// Nothing to edit here; see FIRST-TIME SETUP above.
// ==========================================================

// ==========================================================
// Config
// ==========================================================
// Setup portal
#define AP_SSID         "FamApp-Soundbox"
#define AP_PASSWORD     "soundbox"      // 8+ chars, or "" for an open network
#define WIFI_TIMEOUT    20000UL         // give up joining Wi-Fi after this
#define PORTAL_TIMEOUT  300000UL        // retry after 5 min if nobody configures

#define IMAP_HOST       "imap.gmail.com"
#define IMAP_PORT       993
// Button: press to hear the last payment again.
#define BUTTON_PIN      32

// BOOT button on the ESP32 board (GPIO 0): hold 10 s while running to
// erase every saved setting and restart into setup mode.
#define RESET_PIN       0
#define RESET_HOLD_MS   10000UL
#define RESET_SHOW_MS   3000UL      // start the on-screen countdown after this
#define DEBOUNCE_MS     200


// Uncomment to announce in Hindi instead of English.
// The Google TTS voice language follows automatically.
//#define ANNOUNCE_HINDI

#ifdef ANNOUNCE_HINDI
#define TTS_LANG        "hi"
#else
#define TTS_LANG        "en"
#endif

#define AUDIO_VOLUME    21      // 0..21

// How long the payment screen stays up before the home screen returns
#define PAYMENT_HOLD    15000UL

// OLED
#define OLED_W          128
#define OLED_H          32
#define OLED_ADDR       0x3C
#define PIN_SDA         21
#define PIN_SCL         22

// I2S -> MAX98357A
#define PIN_I2S_BCLK    26
#define PIN_I2S_LRC     25
#define PIN_I2S_DOUT    27

// ==========================================================
// Objects
// ==========================================================
IMAPSession    imap;
Session_Config sessionConfig;
IMAP_Data      imapData;

Audio audio;
Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);

// Settings loaded from flash by loadConfig().
String cfgSsid, cfgWifiPass, cfgGmail, cfgAppPass;

// OLED state: when the payment screen went up (0 = home screen showing),
// and the last amount so the home screen can recall it.
uint32_t paymentShownAt = 0;
float    lastAmount     = -1;
String   deviceIP       = "";

// Defined further down; declared here because setup() attaches it
// and Arduino does not auto-prototype IRAM_ATTR functions.
void IRAM_ATTR onButton();

// Gmail runs in its own task on core 0, so slow polls or reconnects
// never delay the button, screen or speaker on core 1 (loop()).
// Payments found by the Gmail task are handed over through this queue.
QueueHandle_t paymentQueue = nullptr;

// Network state reported by the Gmail task; only loop() draws it.
#define NET_OK          0
#define NET_WIFI_LOST   1
#define NET_GMAIL_LOST  2
volatile int netStatus = NET_OK;


// ==========================================================
// OLED helpers
// ==========================================================
void oledMessage(const char *line1, const char *line2)
{
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);

    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.println(line1);

    if (line2)
    {
        oled.setCursor(0, 16);
        oled.println(line2);
    }

    oled.display();
}


// Home / idle screen. Shows the last payment once there has been
// one, otherwise the IP so the box is easy to identify on the network.
void oledHome()
{
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);

    oled.setCursor(0, 0);
    oled.print("FamApp Soundbox");

    oled.setCursor(0, 12);
    oled.print("Waiting for payment");

    oled.setCursor(0, 24);

    if (lastAmount >= 0)
    {
        oled.print("Last: Rs.");
        oled.print(lastAmount, 2);
    }
    else
    {
        oled.print(deviceIP);
    }

    oled.display();

    paymentShownAt = 0;     // home screen is up
}


// Big amount on the 128x32 panel: small label on top, large number below.
void oledAmount(float amount, const char *label = "PAYMENT RECEIVED")
{
    char buf[24];
    snprintf(buf, sizeof(buf), "Rs.%.2f", amount);

    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);

    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.print(label);

    oled.setTextSize(2);
    oled.setCursor(0, 14);
    oled.print(buf);

    oled.display();

    lastAmount     = amount;
    paymentShownAt = millis();
}


// ==========================================================
// WiFi
// ==========================================================
bool connectWiFi()
{
    Serial.println();
    Serial.print("Connecting to WiFi: ");
    Serial.println(cfgSsid);
    oledMessage("Connecting WiFi", cfgSsid.c_str());

    WiFi.mode(WIFI_STA);
    WiFi.begin(cfgSsid.c_str(), cfgWifiPass.c_str());

    uint32_t t0 = millis();

    while (WiFi.status() != WL_CONNECTED)
    {
        if (millis() - t0 > WIFI_TIMEOUT)
        {
            Serial.println();
            Serial.println("WiFi connect timed out.");
            return false;
        }

        delay(500);
        Serial.print(".");
    }

    Serial.println();
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());

    deviceIP = WiFi.localIP().toString();

    oledMessage("WiFi connected", deviceIP.c_str());
    return true;
}


// ==========================================================
// Parsers
// ==========================================================
float parseAmount(const char *subject)
{
    if (!subject)
        return -1;

    const char *p = subject;

    while (*p && !isdigit((unsigned char)*p))
        p++;

    if (!*p)
        return -1;

    // Copy the number without its thousands separators, so
    // "1,500.00" and Indian-style "1,23,456" parse in full
    // (atof alone stops at the first comma).
    char num[24];
    int  n = 0;

    while (*p && n < (int)sizeof(num) - 1 &&
           (isdigit((unsigned char)*p) || *p == ',' || *p == '.'))
    {
        if (*p != ',')
            num[n++] = *p;
        p++;
    }

    num[n] = '\0';
    return atof(num);
}


// True only if the sender's address is @famapp.in (or a subdomain).
// Gmail's from:famapp.in also matches any address or display name
// that merely contains "famapp.in", e.g. famapp.in.fake@gmail.com.
bool isFromFamApp(const char *from)
{
    if (!from)
        return false;

    String f = String(from);
    f.toLowerCase();

    // "FamApp <noreply@famapp.in>" -> take the part inside <>.
    int lt = f.lastIndexOf('<');

    if (lt >= 0)
    {
        int gt = f.indexOf('>', lt);
        f = f.substring(lt + 1, gt > lt ? gt : f.length());
    }

    f.trim();

    int at = f.lastIndexOf('@');

    if (at < 0)
        return false;

    String domain = f.substring(at + 1);

    return domain == "famapp.in" || domain.endsWith(".famapp.in");
}


// ==========================================================
// Build the sentence to speak -- amount FIRST, so the part the
// listener cares about is heard about a second into the clip
// instead of at its end.
//
// Google reads digits naturally in both languages, so no
// number-to-words conversion is needed. Paise are only
// mentioned when non-zero.
// ==========================================================
// "6 rupees 50 paise" / "6 रुपये 50 पैसे"
String amountPhrase(float amount)
{
    int rupees = (int)amount;
    int paise  = (int)roundf((amount - rupees) * 100.0f);

    if (paise == 100)
    {
        rupees += 1;
        paise = 0;
    }

    String s = String(rupees);

#ifdef ANNOUNCE_HINDI
    s += " रुपये";

    if (paise > 0)
    {
        s += " ";
        s += String(paise);
        s += " पैसे";
    }
#else
    s += (rupees == 1) ? " rupee" : " rupees";

    if (paise > 0)
    {
        s += " ";
        s += String(paise);
        s += " paise";
    }
#endif

    return s;
}


String buildSpeech(float amount)
{
#ifdef ANNOUNCE_HINDI
    return amountPhrase(amount) + " प्राप्त हुए.";
#else
    return amountPhrase(amount) + " received.";
#endif
}


// Button: repeat the last payment.
String buildLastPaymentSpeech(float amount)
{
#ifdef ANNOUNCE_HINDI
    return "पिछला भुगतान, " + amountPhrase(amount) + ".";
#else
    return "Last payment, " + amountPhrase(amount) + ".";
#endif
}


// ==========================================================
// Speak a sentence via Google Translate TTS.
//
// connecttospeech() only starts the stream; the MP3 is decoded
// inside audio.loop(), so we pump that until playback ends.
// A timeout keeps a stalled stream from hanging the box.
// ==========================================================
bool speak(const String &text)
{
    Serial.print("TTS: ");
    Serial.println(text);

    TLOG("TTS: request to Google starting");

    if (!audio.connecttospeech(text.c_str(), TTS_LANG))
    {
        Serial.println("TTS: connecttospeech failed");
        return false;
    }

    unsigned long start = millis();

    // Give the stream a moment to actually start.
    while (!audio.isRunning() && millis() - start < 5000)
    {
        audio.loop();
        delay(1);
    }

    // 15 s cap: a stalled Google stream must not mute the box for long.
    while (audio.isRunning() && millis() - start < 15000)
    {
        audio.loop();
    }

    TLOG("TTS: speech finished");
    Serial.println("TTS: done");
    return true;
}


// Optional callback from the audio library -- useful while debugging.
void audio_info(const char *info)
{
#ifdef TIMING_DEBUG
    // First decoded frame = the moment sound starts.
    if (strstr(info, "syncword found"))
        TLOG("TTS: first audio frame decoded (sound starts)");
#endif

    Serial.print("audio: ");
    Serial.println(info);
}


// ==========================================================
// Beep: at start-up (replaces the spoken "Soundbox ready") and
// before every payment announcement.
//
// Generated locally, so it needs no internet and is instant. It borrows
// the amplifier pins on the ESP32's second I2S port, then hands them
// back to the audio library (setPinout re-attaches them each call).
// ==========================================================
#define BEEP_RATE       44100
#define BEEP_AMPLITUDE  14000   // of 32767; 30000 distorted. Raise/lower to taste

void beepTone(I2SClass &i2s, float freq, uint32_t ms)
{
    const uint32_t total = (BEEP_RATE * ms) / 1000;
    const float    step  = 2.0f * PI * freq / BEEP_RATE;
    const uint32_t ramp  = BEEP_RATE / 200;     // 5 ms fade in/out: no click
    float    phase = 0.0f;
    int16_t  buf[256];
    uint32_t sent  = 0;

    while (sent < total)
    {
        int frames = 0;

        for (int i = 0; i < 256 && sent < total; i += 2, sent++, frames++)
        {
            float env = 1.0f;

            if (sent < ramp)
                env = (float)sent / ramp;
            else if (total - sent < ramp)
                env = (float)(total - sent) / ramp;

            int16_t v  = (int16_t)(sinf(phase) * BEEP_AMPLITUDE * env);
            buf[i]     = v;
            buf[i + 1] = v;

            phase += step;
            if (phase > 2.0f * PI)
                phase -= 2.0f * PI;
        }

        i2s.write((uint8_t *)buf, frames * 2 * sizeof(int16_t));
    }
}


bool audioPinsAttached = false;

void playBeep()
{
    I2SClass i2s;

    i2s.setPins(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);

    if (!i2s.begin(I2S_MODE_STD, BEEP_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO))
    {
        Serial.println("Beep: I2S init failed.");

        if (audioPinsAttached)
            audio.setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
        return;
    }

    beepTone(i2s, 1500, 120);
    beepTone(i2s, 0,     60);       // short gap
    beepTone(i2s, 2000, 180);
    beepTone(i2s, 0,    100);       // let the DMA drain before stopping

    i2s.end();

    // Give the pins back to the speech player.
    if (audioPinsAttached)
        audio.setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
}


// ==========================================================
// Announce: OLED + speaker
// ==========================================================
void announcePayment(float amount)
{
    Serial.println();
    Serial.println("*******************************************");
    Serial.print  ("  PAYMENT RECEIVED: Rs. ");
    Serial.println(amount, 2);
    Serial.println("*******************************************");

    oledAmount(amount);
    TLOG("OLED shows the amount");

    playBeep();
    speak(buildSpeech(amount));

    // Remember it across power cuts for the "last payment" button.
    // Done after speaking so the flash write never delays the voice.
    Preferences p;
    p.begin("soundbox", false);
    p.putFloat("lastamt", amount);
    p.end();
}


// Button: say the last payment again (survives restarts).
void repeatLastPayment()
{
    Serial.println("Button: repeating the last payment.");

    // Instant feedback that the press was heard.
    playBeep();

    if (lastAmount < 0)
    {
        oledMessage("No payment yet", "");
#ifdef ANNOUNCE_HINDI
        speak("अभी तक कोई भुगतान नहीं.");
#else
        speak("No payment yet.");
#endif
        delay(500);
        oledHome();
        return;
    }

    oledAmount(lastAmount, "LAST PAYMENT");
    speak(buildLastPaymentSpeech(lastAmount));
}


bool isPaymentSubject(const String &subject)
{
    String s = subject;
    s.toLowerCase();
    return s.indexOf("you received") >= 0;
}


// ==========================================================
// RFC 2047 header decoding ("=?UTF-8?B?...?=" / "=?UTF-8?Q?...?=").
//
// Raw header fetches return subjects exactly as sent, and a subject
// with a rupee sign is usually encoded. Base64 hides the digits, and
// quoted-printable turns the rupee sign into "=E2=82=B9", whose "2"
// parseAmount() would otherwise read as the amount.
// ==========================================================
int b64Digit(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}


String decodeB64(const String &in)
{
    String out;
    int  bits = 0;
    long acc  = 0;

    for (unsigned i = 0; i < in.length(); i++)
    {
        int v = b64Digit(in[i]);

        if (v < 0)
            continue;               // '=' padding, whitespace

        acc  = (acc << 6) | v;
        bits += 6;

        if (bits >= 8)
        {
            bits -= 8;
            out += (char)((acc >> bits) & 0xFF);
        }
    }

    return out;
}


int hexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}


String decodeQ(const String &in)
{
    String out;

    for (unsigned i = 0; i < in.length(); i++)
    {
        char c = in[i];

        if (c == '_')
            out += ' ';
        else if (c == '=' && i + 2 < in.length() && hexDigit(in[i + 1]) >= 0 && hexDigit(in[i + 2]) >= 0)
        {
            out += (char)(hexDigit(in[i + 1]) * 16 + hexDigit(in[i + 2]));
            i += 2;
        }
        else
            out += c;
    }

    return out;
}


// Decodes every encoded word in a header value. Plain text passes
// through untouched; whitespace between two encoded words is dropped,
// as the RFC requires.
String decodeMimeHeader(const String &in)
{
    String out;
    int  i = 0;
    int  n = in.length();
    bool lastWasEncoded = false;

    while (i < n)
    {
        int start = in.indexOf("=?", i);

        if (start < 0)
        {
            out += in.substring(i);
            break;
        }

        int q1  = in.indexOf('?', start + 2);
        int q2  = q1 >= 0 ? in.indexOf('?', q1 + 1) : -1;
        int end = q2 >= 0 ? in.indexOf("?=", q2 + 1) : -1;

        if (q1 < 0 || q2 != q1 + 2 || end < 0)
        {
            // Not a well-formed encoded word: keep it as plain text.
            out += in.substring(i, start + 2);
            i = start + 2;
            lastWasEncoded = false;
            continue;
        }

        String between = in.substring(i, start);
        String gap     = between;
        gap.trim();

        if (!(lastWasEncoded && gap.length() == 0))
            out += between;

        char   enc  = in[q1 + 1];
        String text = in.substring(q2 + 1, end);

        if (enc == 'B' || enc == 'b')
            out += decodeB64(text);
        else if (enc == 'Q' || enc == 'q')
            out += decodeQ(text);
        else
            out += in.substring(start, end + 2);

        i = end + 2;
        lastWasEncoded = true;
    }

    return out;
}


// Gmail's INTERNALDATE ("17-Oct-2026 16:24:44 +0000") as Unix time.
// Used only to log how long Gmail had an email before we saw it.
time_t internalDateToUnix(const String &s)
{
    struct tm tm = {};
    const char *rest = strptime(s.c_str(), "%d-%b-%Y %H:%M:%S", &tm);

    if (!rest)
        return 0;

    // Days from civil date (timezone-independent, unlike mktime).
    int y = tm.tm_year + 1900;
    int m = tm.tm_mon + 1;
    y -= m <= 2;
    long     era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + tm.tm_mday - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long     days = era * 146097L + (long)doe - 719468L;

    time_t t = (time_t)days * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;

    // "+0530" style offset after the time.
    while (*rest == ' ')
        rest++;

    if ((rest[0] == '+' || rest[0] == '-') && strlen(rest) >= 5)
    {
        int off = ((rest[1] - '0') * 10 + (rest[2] - '0')) * 3600 +
                  ((rest[3] - '0') * 10 + (rest[4] - '0')) * 60;
        t += rest[0] == '+' ? -off : off;
    }

    return t;
}


// ==========================================================
// POLLING ENGINE
//
// Measured on this box: Gmail's IDLE push arrived ~27 s after Gmail
// already had the email, while a plain poll saw it ~3 s after. So the
// box polls instead of waiting for a push.
//
// Each poll is ONE round trip (~0.4 s):
//   UID FETCH <cursor+1>:* (UID INTERNALDATE BODY.PEEK[HEADER.FIELDS (FROM SUBJECT)])
// No new mail: Gmail answers with just the newest old email ("*"
// always matches at least one), which the UID check drops. New mail:
// the same reply already holds its sender and subject, so nothing
// else has to be fetched before announcing.
// ==========================================================
#define POLL_GAP        1000UL      // idle gap between polls
#define MAX_BATCH       10          // payments announced per poll, max

// Highest UID already looked at (FamApp or not). Set from UIDNEXT at
// start-up, so old mail is never announced after a reboot.
uint32_t cursorUID   = 0;
uint32_t uidValidity = 0;

// Consecutive polls that failed. loop() drops a dead session after 3.
int checkFailures = 0;

// Message count from the last "* N EXISTS". A rise with no new UID in
// the same reply means Gmail announced mail the FETCH range had not
// caught yet, so poll again at once.
long knownExists  = -1;
bool pollAgainNow = false;

struct PendingPayment
{
    uint32_t uid;
    String   subject;
    String   idate;
};

PendingPayment pending[MAX_BATCH];
int            pendingCount = 0;
uint32_t       replyMaxUID  = 0;
long           replyExists  = -1;

// Parser state for the message currently streaming in.
bool     inMsg         = false;
bool     msgHasHeaders = false;
uint32_t msgUID        = 0;
String   msgFrom, msgSubject, msgIdate;
String  *lastHeader    = nullptr;


void finishMessage()
{
    inMsg = false;

    // Old mail ("*" echo) or an unsolicited FLAGS update: nothing new.
    if (!msgHasHeaders || msgUID <= cursorUID)
        return;

    if (msgUID > replyMaxUID)
        replyMaxUID = msgUID;

    String subject = decodeMimeHeader(msgSubject);

    if (!isPaymentSubject(subject))
    {
        Serial.print("New mail UID ");
        Serial.print(msgUID);
        Serial.println(" is not a payment.");
        return;
    }

    if (!isFromFamApp(msgFrom.c_str()))
    {
        Serial.print("IGNORED UID ");
        Serial.print(msgUID);
        Serial.print(": payment-like subject but sender is not @famapp.in (possible fake): ");
        Serial.println(msgFrom);
        return;
    }

    if (pendingCount < MAX_BATCH)
    {
        pending[pendingCount].uid     = msgUID;
        pending[pendingCount].subject = subject;
        pending[pendingCount].idate   = msgIdate;
        pendingCount++;
    }
    else
    {
        Serial.print("WARNING: more than MAX_BATCH payments in one poll; UID ");
        Serial.print(msgUID);
        Serial.println(" not announced.");
    }
}


// A header line is "Name: value". Anything else inside a message is a
// folded continuation of the previous header.
bool isHeaderStart(const String &t)
{
    int colon = t.indexOf(':');

    if (colon <= 0 || t.startsWith(" ") || t.startsWith("\t"))
        return false;

    for (int i = 0; i < colon; i++)
        if (t[i] == ' ' || t[i] == '\t')
            return false;

    return true;
}


// Called by the mail library once per reply line of our custom command.
void pollResponse(IMAP_Response res)
{
    String t = String(res.text.c_str());

    while (t.length() && (t.endsWith("\r") || t.endsWith("\n")))
        t.remove(t.length() - 1);

    if (t.startsWith("* ") && t.endsWith(" EXISTS"))
    {
        replyExists = t.substring(2).toInt();
        return;
    }

    if (t.startsWith("* ") && t.indexOf(" FETCH (") > 0)
    {
        if (inMsg)
            finishMessage();

        inMsg      = true;
        msgFrom    = "";
        msgSubject = "";
        msgIdate   = "";
        lastHeader = nullptr;

        int u  = t.indexOf("UID ");
        msgUID = u > 0 ? (uint32_t)t.substring(u + 4).toInt() : 0;

        int d = t.indexOf("INTERNALDATE \"");
        if (d > 0)
            msgIdate = t.substring(d + 14, t.indexOf('"', d + 14));

        msgHasHeaders = t.indexOf("BODY[HEADER") > 0;

        // No literal follows (e.g. a FLAGS-only update): done already.
        if (t.indexOf('{') < 0)
            finishMessage();

        return;
    }

    if (!inMsg)
        return;

    String trimmed = t;
    trimmed.trim();

    if (trimmed == ")")
    {
        finishMessage();
        return;
    }

    if (trimmed.length() == 0)
        return;

    if (isHeaderStart(t))
    {
        String low = t;
        low.toLowerCase();

        if (low.startsWith("from:"))
        {
            msgFrom = t.substring(5);
            msgFrom.trim();
            lastHeader = &msgFrom;
        }
        else if (low.startsWith("subject:"))
        {
            msgSubject = t.substring(8);
            msgSubject.trim();
            lastHeader = &msgSubject;
        }
        else
            lastHeader = nullptr;

        return;
    }

    // Folded continuation of the previous header.
    if (lastHeader)
    {
        *lastHeader += " ";
        *lastHeader += trimmed;
    }
}


// One poll. Returns the number of payments announced, or -1 if the
// command failed.
int pollGmail()
{
    pendingCount = 0;
    replyMaxUID  = 0;
    replyExists  = -1;
    inMsg        = false;

    String cmd = "UID FETCH " + String(cursorUID + 1) +
                 ":* (UID INTERNALDATE BODY.PEEK[HEADER.FIELDS (FROM SUBJECT)])";

    TSTART();

    if (!imap.sendCustomCommand(cmd.c_str(), pollResponse, F("P1")))
    {
        Serial.print("Poll failed: ");
        Serial.println(imap.errorReason());
        checkFailures++;
        return -1;
    }

    if (inMsg)
        finishMessage();

    checkFailures = 0;

    if (replyMaxUID > cursorUID)
        cursorUID = replyMaxUID;

    // Gmail said the mailbox grew but the range had nothing new yet.
    if (replyExists >= 0)
    {
        if (knownExists >= 0 && replyExists > knownExists && replyMaxUID == 0)
            pollAgainNow = true;

        knownExists = replyExists;
    }

    if (pendingCount == 0)
        return 0;

    TLOG("poll reply with new payment received");

    // Oldest first, so the announcements follow the order paid.
    for (int i = 1; i < pendingCount; i++)
        for (int j = i; j > 0 && pending[j - 1].uid > pending[j].uid; j--)
        {
            PendingPayment tmp = pending[j];
            pending[j]         = pending[j - 1];
            pending[j - 1]     = tmp;
        }

    int announced = 0;

    for (int i = 0; i < pendingCount; i++)
    {
        Serial.println();
        Serial.println("--------- NEW PAYMENT EMAIL ---------");
        Serial.print("UID    : "); Serial.println(pending[i].uid);
        Serial.print("Subject: "); Serial.println(pending[i].subject);

        time_t got = internalDateToUnix(pending[i].idate);
        time_t now = time(nullptr);

        if (got > 0 && now > 1577836800)
        {
            Serial.print("Gmail had it ");
            Serial.print((long)(now - got));
            Serial.println(" s before this poll saw it.");
        }

        float amount = parseAmount(pending[i].subject.c_str());

        if (amount < 0)
        {
            Serial.println("WARNING: could not parse an amount from the subject.");
            continue;
        }

        // Hand it to loop(), which beeps, shows and speaks it.
        if (xQueueSend(paymentQueue, &amount, 0) == pdTRUE)
            announced++;
        else
            Serial.println("WARNING: payment queue full; amount dropped.");
    }

    return announced;
}


// Fallback when SELECT gave no UIDNEXT: ask for the newest email's UID.
uint32_t newestSeen = 0;

void newestResponse(IMAP_Response res)
{
    String t = String(res.text.c_str());
    int u = t.indexOf("UID ");

    if (t.startsWith("* ") && t.indexOf(" FETCH (") > 0 && u > 0)
        newestSeen = max(newestSeen, (uint32_t)t.substring(u + 4).toInt());
}


uint32_t newestUID()
{
    newestSeen = 0;
    imap.sendCustomCommand("UID FETCH * (UID)", newestResponse, F("P2"));
    return newestSeen;
}


// Open INBOX and line the cursor up with it. On the first call (and if
// Gmail ever renumbers the mailbox) the cursor starts at UIDNEXT-1, so
// only mail arriving from now on is announced. After a reconnect the
// cursor is kept, so payments that landed while offline are caught by
// the next poll.
bool openInbox()
{
    if (!imap.selectFolder(F("INBOX")))
    {
        Serial.print("Could not open INBOX: ");
        Serial.println(imap.errorReason());
        return false;
    }

    SelectedFolderInfo f = imap.selectedFolder();

    uint32_t validity = (uint32_t)f.uidValidity();
    uint32_t next     = (uint32_t)f.nextUID();

    if (cursorUID == 0 || validity != uidValidity)
    {
        cursorUID = next > 0 ? next - 1 : newestUID();

        // Never start from 0: "1:*" would fetch the whole mailbox.
        if (cursorUID == 0)
        {
            Serial.println("Could not read the newest UID.");
            return false;
        }

        Serial.print("Baseline: watching for UIDs above ");
        Serial.println(cursorUID);
    }

    uidValidity = validity;
    knownExists = (long)f.msgCount();
    return true;
}


// ==========================================================
// SETUP PORTAL
//
// Settings live in flash (Preferences / NVS), so they survive
// power cycles and re-flashing the same sketch.
// ==========================================================
Preferences prefs;
WebServer   server(80);
DNSServer   dns;

String portalNote      = "";
bool   portalNoteError = false;
String scanOptions     = "";


void loadConfig()
{
    prefs.begin("soundbox", true);
    cfgSsid     = prefs.getString("ssid",  "");
    cfgWifiPass = prefs.getString("wpass", "");
    cfgGmail    = prefs.getString("gmail", "");
    cfgAppPass  = prefs.getString("apass", "");
    lastAmount  = prefs.getFloat("lastamt", -1);
    prefs.end();
}


void saveConfig()
{
    prefs.begin("soundbox", false);
    prefs.putString("ssid",  cfgSsid);
    prefs.putString("wpass", cfgWifiPass);
    prefs.putString("gmail", cfgGmail);
    prefs.putString("apass", cfgAppPass);
    prefs.end();
}


bool configComplete()
{
    return cfgSsid.length() > 0 && cfgGmail.length() > 0 && cfgAppPass.length() > 0;
}


String htmlEscape(String v)
{
    v.replace("&", "&amp;");
    v.replace("<", "&lt;");
    v.replace(">", "&gt;");
    v.replace("\"", "&quot;");
    return v;
}


const char PORTAL_PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FamApp Soundbox Setup</title>
<style>
body{font-family:system-ui,sans-serif;background:#f4f4f5;margin:0;padding:16px;color:#18181b}
.card{max-width:420px;margin:0 auto;background:#fff;border-radius:12px;padding:20px;box-shadow:0 1px 4px rgba(0,0,0,.08)}
h1{font-size:20px;margin:0 0 4px}
h2{font-size:14px;text-transform:uppercase;letter-spacing:.05em;color:#71717a;margin:20px 0 8px}
label{display:block;font-size:14px;margin:10px 0 4px}
input{width:100%;box-sizing:border-box;padding:10px;font-size:16px;border:1px solid #d4d4d8;border-radius:8px}
button{width:100%;margin-top:20px;padding:12px;font-size:16px;border:0;border-radius:8px;background:#16a34a;color:#fff}
.note{padding:10px;border-radius:8px;font-size:14px;background:#eff6ff;color:#1e40af}
.err{background:#fef2f2;color:#b91c1c}
.hint{font-size:12px;color:#71717a;margin:4px 0 0}
</style></head><body><div class="card">
<h1>FamApp Soundbox</h1>
<p class="note %NOTECLASS%">%NOTE%</p>
<form method="post" action="/save">
<h2>Wi-Fi</h2>
<label for="ssid">Network name</label>
<input id="ssid" name="ssid" list="nets" value="%SSID%" required autocomplete="off">
<datalist id="nets">%OPTIONS%</datalist>
<label for="wpass">Wi-Fi password</label>
<input id="wpass" name="wpass" type="password" placeholder="%WPH%">
<h2>Gmail</h2>
<label for="gmail">Gmail address</label>
<input id="gmail" name="gmail" type="email" value="%GMAIL%" required>
<label for="apass">Gmail app password</label>
<input id="apass" name="apass" type="password" placeholder="%APH%" autocomplete="off">
<p class="hint">16 letters, from myaccount.google.com/apppasswords (needs 2-Step Verification). Spaces are fine.</p>
<button type="submit">Save and restart</button>
</form></div></body></html>)rawliteral";


String portalPage(const String &ssid, const String &gmail)
{
    String page = FPSTR(PORTAL_PAGE);

    page.replace("%NOTE%",      htmlEscape(portalNote));
    page.replace("%NOTECLASS%", portalNoteError ? "err" : "");
    page.replace("%SSID%",      htmlEscape(ssid));
    page.replace("%GMAIL%",     htmlEscape(gmail));
    page.replace("%OPTIONS%",   scanOptions);
    page.replace("%WPH%", cfgWifiPass.length() ? "Leave blank to keep current" : "");
    page.replace("%APH%", cfgAppPass.length()  ? "Leave blank to keep current" : "");

    return page;
}


void sendMessagePage(const String &title, const String &body)
{
    String page = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<style>body{font-family:system-ui,sans-serif;padding:24px;text-align:center}</style></head><body><h2>";
    page += htmlEscape(title);
    page += "</h2><p>";
    page += htmlEscape(body);
    page += "</p></body></html>";

    server.send(200, "text/html", page);
}


void handleRoot()
{
    server.send(200, "text/html", portalPage(cfgSsid, cfgGmail));
}


void handleSave()
{
    String ssid  = server.arg("ssid");
    String wpass = server.arg("wpass");
    String gmail = server.arg("gmail");
    String apass = server.arg("apass");

    ssid.trim();
    gmail.trim();
    apass.replace(" ", "");     // Google shows it as four groups of four

    // Blank password fields keep what is stored. For Wi-Fi only when the
    // network is unchanged -- a new SSID with no password is an open network.
    if (wpass.length() == 0 && ssid == cfgSsid)
        wpass = cfgWifiPass;

    if (apass.length() == 0)
        apass = cfgAppPass;

    String problem = "";

    if (ssid.length() == 0)
        problem = "Enter the Wi-Fi network name.";
    else if (gmail.indexOf('@') < 1)
        problem = "Enter a valid Gmail address.";
    else if (apass.length() != 16)
        problem = "The Gmail app password should be 16 letters (spaces removed).";

    if (problem.length())
    {
        portalNote      = problem;
        portalNoteError = true;
        server.send(200, "text/html", portalPage(ssid, gmail));
        return;
    }

    cfgSsid     = ssid;
    cfgWifiPass = wpass;
    cfgGmail    = gmail;
    cfgAppPass  = apass;
    saveConfig();

    Serial.println("Settings saved. Restarting...");

    sendMessagePage("Saved", "The soundbox is restarting and will join " + ssid +
                             ". You can reconnect your phone to your normal Wi-Fi.");

    delay(2000);
    ESP.restart();
}


// Phones probe URLs like /generate_204 to detect a captive portal.
// Redirecting everything unknown to the form makes the page pop up.
void handleNotFound()
{
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
}


void oledLines(const char *l1, const char *l2, const char *l3, const char *l4)
{
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);

    oled.setCursor(0, 0);  oled.print(l1);
    oled.setCursor(0, 8);  oled.print(l2);
    oled.setCursor(0, 16); oled.print(l3);
    oled.setCursor(0, 24); oled.print(l4);

    oled.display();
}


// Start the box's own Wi-Fi network and serve the setup form.
// Never returns: saving restarts the ESP32. If canTimeout is set
// (settings exist, a connection just failed), the box restarts after
// PORTAL_TIMEOUT so a brief outage does not strand it in setup mode.
void runPortal(const String &why, bool canTimeout)
{
    Serial.println();
    Serial.println("=== SETUP PORTAL ===");
    Serial.println(why);

    portalNote      = why;
    portalNoteError = canTimeout;

    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP_STA);
    delay(100);

    // Scan once so the form can suggest nearby networks.
    scanOptions = "";
    int n = WiFi.scanNetworks();

    for (int i = 0; i < n; i++)
    {
        String opt = "<option value=\"" + htmlEscape(WiFi.SSID(i)) + "\">";

        if (WiFi.SSID(i).length() && scanOptions.indexOf(opt) < 0)
            scanOptions += opt;
    }

    WiFi.softAP(AP_SSID, AP_PASSWORD);
    IPAddress ip = WiFi.softAPIP();

    dns.start(53, "*", ip);

    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.onNotFound(handleNotFound);
    server.begin();

    Serial.print("Join Wi-Fi '");
    Serial.print(AP_SSID);
    Serial.print("' (password '");
    Serial.print(AP_PASSWORD);
    Serial.print("') and open http://");
    Serial.println(ip);

    oledLines("SETUP MODE",
              "WiFi:" AP_SSID,
              "Pass:" AP_PASSWORD,
              "Open 192.168.4.1");

    uint32_t t0 = millis();

    while (true)
    {
        dns.processNextRequest();
        server.handleClient();

        if (canTimeout && millis() - t0 > PORTAL_TIMEOUT && WiFi.softAPgetStationNum() == 0)
        {
            Serial.println("Nobody configured the box; restarting to retry.");
            ESP.restart();
        }

        delay(2);
    }
}


// ==========================================================
void setup()
{
    Serial.begin(115200);
    delay(200);     // was 2000; only there so a monitor can attach

    Serial.println();
    Serial.println("==============================================");
    Serial.println(" ESP32 FAMAPP SOUNDBOX  (Google TTS, polling)");
    Serial.println("==============================================");

    // ---- OLED ----
    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(400000);      // 400 kHz: a full screen update ~4x faster

    if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR))
    {
        Serial.println("OLED not found at 0x3C -- check wiring/address.");
    }
    else
    {
        oled.clearDisplay();
        oled.display();
        oledMessage("FamApp Soundbox", "starting...");
    }

    // ---- Button ----
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(RESET_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), onButton, FALLING);
    delay(20);
    Serial.print("Button pin GPIO ");
    Serial.print(BUTTON_PIN);
    Serial.println(digitalRead(BUTTON_PIN) == HIGH ? " reads HIGH (released) - OK"
                                                    : " reads LOW (pressed?) - if not held, check the wiring");

    // ---- Settings ----
    loadConfig();
    delay(50);      // let the pull-up settle before reading the button

    // Holding the button while powering on opens the setup portal.
    if (digitalRead(BUTTON_PIN) == LOW)
        runPortal("Setup opened because the button was held at start-up.", false);

    if (!configComplete())
        runPortal("No settings saved yet. Enter them below to get started.", false);

    if (!connectWiFi())
        runPortal(String("Could not join Wi-Fi '") + cfgSsid + "'. Check the network name and password.", true);

    // Set the clock from NTP once. With no valid time, the mail library's
    // clock check blocks ~5 s per call (getLocalTime timeout), which cost
    // ~10 s on every Gmail command. Google's time server first: it answered
    // fastest here; pool.ntp.org took up to ~5 s.
    configTime(19800, 0, "time.google.com", "time.cloudflare.com", "pool.ntp.org");   // IST (UTC+5:30)
    {
        uint32_t t0 = millis();

        while (time(nullptr) < 1577836800 && millis() - t0 < 10000)
            delay(50);

        Serial.print(time(nullptr) >= 1577836800 ? "Clock set from NTP in " : "NTP clock sync FAILED after ");
        Serial.print(millis() - t0);
        Serial.println(" ms");
    }

    MailClient.networkReconnect(true);

    sessionConfig.server.host_name = IMAP_HOST;
    sessionConfig.server.port      = IMAP_PORT;
    sessionConfig.login.email      = cfgGmail.c_str();
    sessionConfig.login.password   = cfgAppPass.c_str();

    // Same servers for the library, so it can resync the clock itself
    // if it ever finds it invalid.
    sessionConfig.time.ntp_server  = F("time.google.com,pool.ntp.org");
    sessionConfig.time.gmt_offset  = 5.5;

    // Nothing is ever saved to a file; skips the library's LittleFS
    // mount attempt (it failed with "Corrupted dir pair" on every login).
    imapData.storage.type = esp_mail_file_storage_type_none;

#ifdef TIMING_DEBUG
    imap.debug(1);
#else
    imap.debug(0);
#endif

    // TCP keep-alive: notices a silently dead link (router reboot).
    imap.keepAlive(5, 5, 1);

    Serial.println("Connecting to Gmail IMAP...");
    oledMessage("Connecting", "Gmail...");

    if (!imap.connect(&sessionConfig, &imapData))
    {
        Serial.println("GMAIL CONNECTION FAILED");
        Serial.println(imap.errorReason());
        oledMessage("Gmail FAILED", "opening setup...");
        delay(2000);

        runPortal(String("Gmail login failed (") + imap.errorReason() +
                  "). Check the Gmail address and app password.", true);
    }

    Serial.println("Gmail connected.");

    // Baseline: one SELECT gives UIDNEXT, so old mail is never
    // announced after a reboot. (Replaces the old ~5 s Gmail search.)
    if (!openInbox())
    {
        oledMessage("Gmail FAILED", "opening setup...");
        delay(2000);
        runPortal("Gmail connected but the INBOX could not be opened.", true);
    }

    oledHome();

    // Beep last, so it really means "ready to announce payments".
    playBeep();

    // ---- Audio (after the beep: the library takes the amp pins) ----
    audio.setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
    audioPinsAttached = true;
    audio.setVolume(AUDIO_VOLUME);

    // Volume 21 is the library's maximum; boost the speech further with
    // its tone control (dB: low, mid, high). Mid carries the voice.
    // Lower these if the speech sounds distorted.
    audio.setTone(3, 6, 3);

    // Ignore anything the interrupt latched during start-up: the pin
    // settling and the boot beep produced a false press here.
    clearButtonLatch();

    // Hand Gmail to its own task on core 0 (Wi-Fi also lives there);
    // loop() on core 1 now only does button, screen and sound.
    paymentQueue = xQueueCreate(MAX_BATCH, sizeof(float));
    xTaskCreatePinnedToCore(gmailTask, "gmail", 20480, nullptr, 1, nullptr, 0);

    Serial.print("Polling Gmail every ~");
    Serial.print(POLL_GAP);
    Serial.println(" ms. Press the button to hear the last payment.");
}


// The button is latched by an interrupt, so a press made while a
// poll or an announcement is running is remembered, not lost.
volatile bool     buttonFlag   = false;
volatile uint32_t lastPressISR = 0;

void clearButtonLatch()
{
    buttonFlag = false;
}


void IRAM_ATTR onButton()
{
    uint32_t now = millis();

    if (now - lastPressISR > DEBOUNCE_MS && digitalRead(BUTTON_PIN) == LOW)
    {
        lastPressISR = now;
        buttonFlag   = true;
    }
}


// Called from loop() while the Gmail session is down. Retries every
// RECONNECT_GAP instead of giving up, so a Wi-Fi blip or Gmail closing
// the session no longer leaves the box silently deaf.
#define RECONNECT_GAP   10000UL

// Returns true once the session is back up.
bool reconnectGmail()
{
    static uint32_t lastTry = 0;

    if (lastTry != 0 && millis() - lastTry < RECONNECT_GAP)
        return false;

    lastTry = millis();

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("WiFi lost; reconnecting...");
        netStatus = NET_WIFI_LOST;
        WiFi.reconnect();
        return false;
    }

    Serial.println("Gmail connection lost; reconnecting...");
    netStatus = NET_GMAIL_LOST;

    if (imap.connect(&sessionConfig, &imapData) && openInbox())
    {
        Serial.println("Gmail reconnected.");
        checkFailures = 0;
        lastTry       = 0;
        netStatus     = NET_OK;
        return true;
    }

    Serial.print("Reconnect failed: ");
    Serial.println(imap.errorReason());
    imap.closeSession();
    return false;
}


// Erase Wi-Fi, Gmail, app password and the saved last payment, then
// restart. With nothing saved, setup() opens the setup portal.
void factoryReset()
{
    Serial.println("BOOT held 10 s: erasing all settings.");
    oledMessage("Erasing settings", "restarting...");
    playBeep();

    Preferences p;
    p.begin("soundbox", false);
    p.clear();
    p.end();

    WiFi.disconnect(true, true);    // also forget the Wi-Fi the core stored

    delay(1000);
    ESP.restart();
}


// Watches the BOOT button; returns true while it is being held, so
// loop() can pause other work and keep the countdown on screen.
bool checkFactoryReset()
{
    static uint32_t heldSince = 0;
    static int      lastShown = -1;

    if (digitalRead(RESET_PIN) == HIGH)
    {
        if (heldSince != 0)
        {
            heldSince = 0;

            if (lastShown >= 0)             // countdown was showing
            {
                Serial.println("BOOT released: reset cancelled.");
                oledHome();
            }

            lastShown = -1;
        }

        return false;
    }

    if (heldSince == 0)
        heldSince = millis();

    uint32_t held = millis() - heldSince;

    if (held >= RESET_HOLD_MS)
        factoryReset();                     // does not return

    if (held >= RESET_SHOW_MS)
    {
        int left = (int)((RESET_HOLD_MS - held + 999) / 1000);

        if (left != lastShown)
        {
            lastShown = left;

            char line[24];
            snprintf(line, sizeof(line), "Hold to reset %d", left);
            oledMessage("ERASE SETTINGS?", line);
        }
    }

    return true;
}


void loop()
{

    // BOOT held: count down to a factory reset, nothing else meanwhile.
    if (checkFactoryReset())
    {
        delay(20);
        return;
    }

    // Wiring check: log every change of the raw button level.
    static int      btnLevel   = -1;
    static uint32_t btnChanged = 0;
    int level = digitalRead(BUTTON_PIN);

    if (level != btnLevel && millis() - btnChanged > 30)
    {
        if (btnLevel != -1)
            Serial.println(level == LOW ? "Button pin: LOW  (pressed)" : "Button pin: HIGH (released)");

        btnLevel   = level;
        btnChanged = millis();
    }

    // Drop the payment screen back to home once it has had its time.
    if (paymentShownAt != 0 && millis() - paymentShownAt > PAYMENT_HOLD)
        oledHome();

    // Network status from the Gmail task (only this core draws).
    static int shownStatus = NET_OK;
    int st = netStatus;

    if (st != shownStatus)
    {
        shownStatus = st;

        if (st == NET_WIFI_LOST)
            oledMessage("WiFi lost", "reconnecting...");
        else if (st == NET_GMAIL_LOST)
            oledMessage("Gmail lost", "reconnecting...");
        else if (paymentShownAt == 0)
            oledHome();
    }

    // Payments first: they matter more than a repeat request.
    float amount;

    if (xQueueReceive(paymentQueue, &amount, 0) == pdTRUE)
        announcePayment(amount);

    // Works offline too: the amount is stored on the box.
    if (buttonFlag)
    {
        repeatLastPayment();

        // Presses made while it was speaking should not replay it again.
        buttonFlag = false;
    }

    audio.loop();
    delay(1);
}


// ==========================================================
// Gmail task (core 0): reconnects and polls forever. Never touches the
// OLED or the speaker; payments go to loop() through paymentQueue.
// ==========================================================
void gmailTask(void *)
{
    uint32_t lastPoll = 0;

    for (;;)
    {
        // Several failed polls in a row: the session is dead even if
        // it still claims to be connected. Close it so it reconnects.
        if (checkFailures >= 3)
        {
            Serial.println("Gmail polls keep failing; dropping the session.");
            imap.closeSession();
            checkFailures = 0;
        }

        if (!imap.connected())
        {
            // The cursor survives the outage, so the first poll after
            // the reconnect picks up anything that landed meanwhile.
            if (reconnectGmail())
                lastPoll = 0;
            else if (netStatus == NET_OK)
                netStatus = NET_GMAIL_LOST;

            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (pollAgainNow || lastPoll == 0 || millis() - lastPoll >= POLL_GAP)
        {
            pollAgainNow = false;
            pollGmail();
            lastPoll = millis();
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

