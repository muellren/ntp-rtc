// Based on NTP-client of pico-examples and the
// galactic_unicorn library from pimoroni-pico.
//
// (c) 2022 Raspberry Pi Ltd.
// (c) 2023 Rene Mueller, Zofingen, Switzerland

#include <cinttypes>
#include <string.h>
#include <time.h>

#include "hardware/rtc.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "pico/cyw43_arch.h"
#include "pico/util/datetime.h"
#include "pico/stdlib.h"
#include "libraries/pico_graphics/pico_graphics.hpp"
#include "galactic_unicorn.hpp"
#include "digits.hpp"

#define NTP_SERVER "pool.ntp.org"
#define NTP_MSG_LEN 48
#define NTP_PORT 123
#define NTP_DELTA 2208988800  // seconds between 1 Jan 1900 and 1 Jan 1970
#define NTP_POLL_INTERVAL (60 * 1000)
#define NTP_RESEND_INTERVAL (10 * 1000)
#define UTC_OFFSET_SECONDS (2 * 3600)

using pimoroni::PicoGraphics_PenRGB888;
using pimoroni::GalacticUnicorn;
using pimoroni::Point;
using pimoroni::Rect;

struct NTP_T {
  ip_addr_t       ntp_server_address; //!< looked-up IP address of a NTP server in the pool
  bool            dns_request_sent;   //!< DNS request was sent and reply receive (may have failed)
  struct udp_pcb *ntp_pcb;            //!< UDP Protocol Control Block
  absolute_time_t ntp_poll_time;      //!< Time for next NTP poll
  alarm_id_t      ntp_resend_alarm;   //!< Alarm for resending NTP request in case request UDP package is lost
};

struct Color {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
};

constexpr int num_digits = 6;
constexpr int digit_width = 7;
constexpr int digit_height = 11;
constexpr Color font_color = {.red = 200, .green = 190, .blue = 150 };
constexpr Color colon_color = {.red = 240, .green = 20, .blue = 5 };
constexpr int extra_space = 3;
constexpr float initial_brightness = 0.5f;
constexpr int update_interval_ms = 25;
constexpr int updates_per_tick = 40;


bool rtc_set = false;
PicoGraphics_PenRGB888 graphics(53, 11, nullptr);
GalacticUnicorn galactic_unicorn;
uint8_t current_digits[num_digits];
uint8_t next_digits[num_digits];
datetime_t shown_datetime;
int anim_updates_remaining = 0;

void write_text(const std::string_view &text) {
  graphics.set_pen(0, 0, 0);
  graphics.clear();
  graphics.set_pen(255, 255, 255);
  graphics.text(text, Point(0, 2), -1, 0.55);
  galactic_unicorn.update(&graphics);
}

// Called with response of NTP request
static void ntp_result(NTP_T *state, int status, time_t *result) {
  if (status == 0 && result) {
    struct tm *local = localtime(result);
    printf("got NTP response: %02d/%02d/%04d %02d:%02d:%02d\n", local->tm_mday,
           local->tm_mon + 1, local->tm_year + 1900, local->tm_hour, local->tm_min,
           local->tm_sec);
    datetime_t t = {
      .year = static_cast<int16_t>(local->tm_year + 1900),
      .month = static_cast<int8_t>(local->tm_mon + 1),
      .day = static_cast<int8_t>(local->tm_mday),
      .hour = static_cast<int8_t>(local->tm_hour),
      .min = static_cast<int8_t>(local->tm_min),
      .sec = static_cast<int8_t>(local->tm_sec)
    };
    rtc_set_datetime(&t);
    rtc_set = true;
    write_text("NTP ok");
  }

  if (state->ntp_resend_alarm > 0) {
    cancel_alarm(state->ntp_resend_alarm);
    state->ntp_resend_alarm = 0;
  }
  state->ntp_poll_time = make_timeout_time_ms(NTP_POLL_INTERVAL);
  state->dns_request_sent = false;
}

static int64_t ntp_failed_handler(alarm_id_t id, void *user_data);

// Submit NTP request via UDP
static void ntp_request(NTP_T *state) {
  // cyw43_arch_lwip_begin/end should be used around calls into lwIP to ensure
  // correct locking. You can omit them if you are in a callback from lwIP. Note
  // that when using pico_cyw_arch_poll these calls are a no-op and can be
  // omitted, but it is a good practice to use them in case you switch the
  // cyw43_arch type later.
  cyw43_arch_lwip_begin();
  struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, NTP_MSG_LEN, PBUF_RAM);
  uint8_t *req = (uint8_t *)p->payload;
  memset(req, 0, NTP_MSG_LEN);
  req[0] = 0x1b;
  udp_sendto(state->ntp_pcb, p, &state->ntp_server_address, NTP_PORT);
  pbuf_free(p);
  cyw43_arch_lwip_end();
}

static int64_t ntp_failed_handler(alarm_id_t id, void *user_data) {
  NTP_T *state = (NTP_T *)user_data;
  printf("NTP request failed\n");
  write_text("NTP failed");
  ntp_result(state, -1, NULL);
  return 0;
}

// Callback with DNS response
static void ntp_dns_found(const char *hostname, const ip_addr_t *ipaddr,
                          void *arg) {
  NTP_T *state = (NTP_T *)arg;
  if (ipaddr) {
    state->ntp_server_address = *ipaddr;
    printf("NTP address %s\n", ipaddr_ntoa(ipaddr));
    ntp_request(state);
  } else {
    printf("NTP DNS request failed\n");
    write_text("DNS failed");
    ntp_result(state, -1, NULL);
  }
}

// NTP data received
static void ntp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                     const ip_addr_t *addr, u16_t port) {
  NTP_T *state = (NTP_T *)arg;
  uint8_t mode = pbuf_get_at(p, 0) & 0x7;
  uint8_t stratum = pbuf_get_at(p, 1);
  // Check the result
  if (ip_addr_cmp(addr, &state->ntp_server_address) && port == NTP_PORT &&
      p->tot_len == NTP_MSG_LEN && mode == 0x4 && stratum != 0) {
    uint8_t seconds_buf[4] = {0};
    pbuf_copy_partial(p, seconds_buf, sizeof(seconds_buf), 40);
    uint32_t seconds_since_1900 = seconds_buf[0] << 24 | seconds_buf[1] << 16 |
                                  seconds_buf[2] << 8 | seconds_buf[3];
    uint32_t seconds_since_1970 = seconds_since_1900 - NTP_DELTA;
    time_t epoch = seconds_since_1970 + UTC_OFFSET_SECONDS;
    ntp_result(state, 0, &epoch);
  } else {
    printf("invalid NTP response\n");
    write_text("bad NTP");
    ntp_result(state, -1, nullptr);
  }
  pbuf_free(p);
}

// Initialisation of NTP client
static NTP_T *ntp_init(void) {
  NTP_T *state = (NTP_T *)calloc(1, sizeof(NTP_T));
  if (!state) {
    printf("failed to allocate NTP state\n");
    return NULL;
  }
  state->ntp_pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
  if (!state->ntp_pcb) {
    printf("failed to create PCB\n");
    free(state);
    return NULL;
  }
  udp_recv(state->ntp_pcb, ntp_recv, state);
  return state;
}

void animate_display() {
  graphics.set_pen(0, 0, 0);
  graphics.clear();
  graphics.set_pen(font_color.red, font_color.green, font_color.blue);
  for (int digit = num_digits - 1; digit >= 0; digit--) {
    if (next_digits[digit] == current_digits[digit]) {
      const char* digit_ptr = &digits[current_digits[digit] * digit_width * digit_height];
      int left_pos = digit * (digit_width + 1) + (digit / 2) * extra_space;
      for (int y = 0; y < digit_height; y++) {
        for (int x = 0; x < digit_width; x++) {
          if (*digit_ptr == '0') {
            graphics.pixel(Point(left_pos + x, y));
          }
          digit_ptr += 1;
        }
      }
    } else {
      // animate digit flipping
      const char* current_digit_ptr = &digits[current_digits[digit] * digit_width * digit_height];
      const char* next_digit_ptr = &digits[next_digits[digit] * digit_width * digit_height];
      int left_pos = digit * (digit_width + 1) + (digit / 2) * extra_space;
      for (int y = 0; y < digit_height; y++) {
        const char* digit_ptr = (y < anim_updates_remaining)
          ? (current_digit_ptr + (y + digit_height - anim_updates_remaining) * digit_width)
          : (next_digit_ptr + (y - anim_updates_remaining) * digit_width);
        for (int x = 0; x < digit_width; x++) {
          if (*digit_ptr == '0') {
            graphics.pixel(Point(left_pos + x, y));
          }
          digit_ptr += 1;
        }
      }
    }
  }
  graphics.set_pen(colon_color.red, colon_color.green, colon_color.blue);
  for (int hdot = 0; hdot < 2; hdot++) {
    int x = 2 * (digit_width + 1) * (hdot + 1) + 3 * hdot;
    for (int vdot = 0; vdot < 2; vdot++) {
      int y = 2 + 5 * vdot;
      graphics.rectangle(Rect(x, y, 2, 2));
    }
  }
  galactic_unicorn.update(&graphics);
}

// Runs forever
void run_ntp_main() {
  NTP_T *state = ntp_init();
  if (state == nullptr) {
    return;
  }

  while (true) {
    if(galactic_unicorn.is_pressed(galactic_unicorn.SWITCH_BRIGHTNESS_UP)) {
      galactic_unicorn.adjust_brightness(+0.01);
    }
    if(galactic_unicorn.is_pressed(galactic_unicorn.SWITCH_BRIGHTNESS_DOWN)) {
      galactic_unicorn.adjust_brightness(-0.01);
    }

    if ((absolute_time_diff_us(get_absolute_time(), state->ntp_poll_time) < 0) &&
                              !state->dns_request_sent) {
      // Set alarm in case udp requests are lost
      state->ntp_resend_alarm =
          add_alarm_in_ms(NTP_RESEND_INTERVAL, ntp_failed_handler, state, true);

      // cyw43_arch_lwip_begin/end should be used around calls into lwIP to
      // ensure correct locking. You can omit them if you are in a callback from
      // lwIP. Note that when using pico_cyw_arch_poll these calls are a no-op
      // and can be omitted, but it is a good practice to use them in case you
      // switch the cyw43_arch type later.
      cyw43_arch_lwip_begin();
      int err = dns_gethostbyname(NTP_SERVER, &state->ntp_server_address,
                                  ntp_dns_found, state);
      cyw43_arch_lwip_end();

      state->dns_request_sent = true;
      if (err == ERR_OK) {
        ntp_request(state);  // Cached result
      } else if (err !=
                 ERR_INPROGRESS) {  // ERR_INPROGRESS means expect a callback
        printf("dns request failed\n");
        ntp_result(state, -1, NULL);
      }
    }

    // Periodically poll from main loop (not from a timer interrupt) to check for Wi-Fi
    // driver or lwIP work that needs to be done.
    cyw43_arch_poll();
    // Poll as often as you like, however if you have nothing else to do
    // you can choose to sleep until either a specified time, or
    // cyw43_arch_poll() has work to do:
    if (!rtc_set) {
      cyw43_arch_wait_for_work_until(
        state->dns_request_sent ? at_the_end_of_time : state->ntp_poll_time);
    } else {
      datetime_t t;
      rtc_get_datetime(&t);
      if (t.sec != shown_datetime.sec ||
          t.min != shown_datetime.min ||
          t.hour != shown_datetime.hour) {
        next_digits[0] = t.hour / 10;
        next_digits[1] = t.hour % 10;
        next_digits[2] = t.min / 10;
        next_digits[3] = t.min % 10;
        next_digits[4] = t.sec / 10;
        next_digits[5] = t.sec % 10;
        shown_datetime = t;
        anim_updates_remaining = digit_height;
      }
      if (anim_updates_remaining == 0) {
        current_digits[0] = next_digits[0];
        current_digits[1] = next_digits[1];
        current_digits[2] = next_digits[2];
        current_digits[3] = next_digits[3];
        current_digits[4] = next_digits[4];
        current_digits[5] = next_digits[5];
      } else {
        anim_updates_remaining -= 1;
      }
      animate_display();
      sleep_ms(update_interval_ms);
    }
  }
  free(state);
}


int main() {
  stdio_init_all();
  galactic_unicorn.init();
  galactic_unicorn.set_brightness(initial_brightness);

  graphics.set_font("bitmap8");
  graphics.set_pen(0, 0, 0);
  graphics.clear();
  galactic_unicorn.update(&graphics);

  write_text("NTP RTC");
  sleep_ms(10000);

  printf("ntp_rtc\n");
  rtc_init();
  printf("RTC: initialized\n");

  if (cyw43_arch_init()) {
    printf("cyw43: failed to initialise\n");
    return 1;
  }
  cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
  printf("cyw43: initialized\n");

  cyw43_arch_enable_sta_mode();
  printf("enabled STA mode, connecting to WiFi...\n");
  write_text("connecting");

  if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD,
                                         CYW43_AUTH_WPA2_AES_PSK, 10000)) {
    printf("failed to connect\n");
    return 1;
  }
  printf("WiFi connected!\n");
  write_text("Getting NTP");
  cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
  run_ntp_main();
  cyw43_arch_deinit();
  return 0;
}
