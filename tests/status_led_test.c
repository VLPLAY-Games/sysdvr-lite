#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include "../src/status_led.c"

static void temperature(int value)
{
    FILE *f = fopen(thermal_path, "w");
    assert(f);
    fprintf(f, "%d\n", value);
    assert(!fclose(f));
}

static void expect_file(const char *name, const char *value)
{
    char path[512], text[64] = "";
    snprintf(path, sizeof path, "%s/%s", led_dir, name);
    FILE *f = fopen(path, "r");
    assert(f);
    assert(fgets(text, sizeof text, f));
    fclose(f);
    assert(!strcmp(text, value));
}

int main(void)
{
    char dir[] = "/tmp/sysdvr-led-test-XXXXXX";
    assert(mkdtemp(dir));
    led_dir = dir;
    snprintf(thermal_path, sizeof thermal_path, "%s/temp", dir);
    uint64_t now = 10000000000ull;
    temperature(79000);
    assert(!update_led(now));
    expect_file("trigger", "timer");
    expect_file("delay_on", "500");
    expect_file("delay_off", "500");
    atomic_store(&last_frame, now);
    assert(!update_led(now));
    expect_file("trigger", "default-on");
    temperature(80000);
    assert(!update_led(now));
    expect_file("trigger", "timer");
    expect_file("delay_on", "125");
    expect_file("delay_off", "125");
    temperature(76001);
    assert(!update_led(now));
    assert(current_mode == HOT);
    /* Losing the sensor must not clear an already raised overheat indication. */
    assert(!unlink(thermal_path));
    assert(!update_led(now));
    assert(current_mode == HOT);
    temperature(76000);
    assert(!update_led(now));
    expect_file("trigger", "default-on");
    assert(!update_led(now + 3000000000ull));
    expect_file("trigger", "timer");
    expect_file("delay_on", "500");
    atomic_store(&last_frame, now);
    assert(!update_led(now));
    status_led_waiting();
    assert(!update_led(now));
    assert(current_mode == WAITING);
    temperature(81000);
    assert(!update_led(now));
    assert(current_mode == HOT);
    temperature(75000);
    assert(!update_led(now));
    assert(current_mode == WAITING);
    const char *files[] = { "trigger", "delay_on", "delay_off", "temp" };
    for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) {
        char path[512]; snprintf(path, sizeof path, "%s/%s", dir, files[i]);
        assert(!unlink(path));
    }
    assert(!rmdir(dir));
    puts("status LED tests passed");
}
