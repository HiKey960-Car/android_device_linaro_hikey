#include <cutils/log.h>

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>

#include <sys/ioctl.h>
#include <sys/types.h>

#include <sys/socket.h>
#include <sys/un.h>

#include <hardware/lights.h>

/* Linux */
#include <linux/types.h>
#include <linux/input.h>
#include <linux/hidraw.h>

/* Unix */
#include <sys/stat.h>

/* C */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

const char *bus_str(int bus);

#define MAX_PATH_SIZE 80

static pthread_once_t g_init = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

char const*const LCD_FILE = "/dev/backlight";

int hid_fd = -1;
unsigned char lastHID = 0x20;

void init_globals(void){
    // init the mutex
    pthread_mutex_init(&g_lock, NULL);
}

const char *bus_str(int bus){
    switch (bus) {
    case BUS_USB:
        return "USB";
        break;
    case BUS_HIL:
        return "HIL";
        break;
    case BUS_BLUETOOTH:
        return "Bluetooth";
        break;
    case BUS_VIRTUAL:
        return "Virtual";
        break;
    default:
        return "Other";
        break;
    }
}

int openhid(char *device){
    int fd;
    int i, res, desc_size = 0;
    char buf[256];
    struct hidraw_report_descriptor rpt_desc;
    struct hidraw_devinfo info;

    /* Open the Device with non-blocking reads. In real life,
       don't use a hard coded path; use libudev instead. */
    fd = open(device, O_RDWR|O_NONBLOCK);

    if (fd < 0) {
        ALOGE("Unable to open device");
        return -1;
    }

    memset(&rpt_desc, 0x0, sizeof(rpt_desc));
    memset(&info, 0x0, sizeof(info));
    memset(buf, 0x0, sizeof(buf));

    /* Get Report Descriptor Size */
    res = ioctl(fd, HIDIOCGRDESCSIZE, &desc_size);
    if (res < 0)
        ALOGE("HIDIOCGRDESCSIZE");
    else
        ALOGD("Report Descriptor Size: %d\n", desc_size);

    if (desc_size != 46){
        close(fd);
        return -1;
    }

    /* Get Raw Info */
    res = ioctl(fd, HIDIOCGRAWINFO, &info);
    if (res < 0) {
        ALOGE("HIDIOCGRAWINFO");
    } else {
        ALOGD("Raw Info:\n");
        ALOGD("\tbustype: %d (%s)\n", info.bustype, bus_str(info.bustype));
        ALOGD("\tvendor: 0x%04hx\n", info.vendor);
        ALOGD("\tproduct: 0x%04hx\n", info.product);
    }

    if (info.vendor != 0x04d8){
        ALOGE("INFO.VENDOR: 0x%04hx", info.vendor);
        close(fd);
        return -1;
    }

    /*TODO not sure why this doesn't work...
    if (info.product != 0xf723
            && info.product != 0xf724
            && info.product != 0x003f){
        ALOGE("INFO.PRODUCT: 0x%04hx", info.product);
        close(fd);
        return -1;
    }*/

    return fd;
}

void writehid(unsigned char value){
    unsigned char buf[16];
    int res,i;

    /*
     * value in the range 0-18 inclusive.
     * command byte defined as follows;
     * b7 b6 b5 b4 b3 b2 b1 b0
     * b0: unused
     * b1: auto-brightness on/off
     * b2: set max brightness
     * b3: set min brightness
     * b4: backlight on/off
     * b5: set brightness to val
     * b6: brightness--
     * b7: brightness++
     */

    buf[0] = 0x0;   // Report ID (0)
    buf[1] = 0x20;  // Command (0x20 - Set Backlight)
    buf[2] = value; // Backlight value for command Set Backlight

    ALOGD("HID writing bytes: %02X,%02X,%02X",buf[0],buf[1],buf[2]);

    res = write(hid_fd, buf, 3);
    if (res < 0) {
        ALOGE("Write error: %d\n", errno);
    } else {
        ALOGD("write() wrote %d bytes\n", res);
    }

    // Get a report from the device * /
    res = read(hid_fd, buf, 16);
    if (res < 0) {
        ALOGE("Read error");
    } else {
        ALOGD("read() read %d bytes:\n\t", res);
        for (i = 0; i < res; i++)
            ALOGD("%hhx ", buf[i]);
    }
}

static int write_int(char const* path, int value){
    static int already_warned = 0;

    struct sockaddr_un addr;
    char buf[100];
    int fd,rc;

    if ( (fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
      perror("socket error");
      return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path)-1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
      perror("connect error");
      return -1;
    }

    if (fd >= 0) {
        unsigned char buffer[2];
        buffer[0] = (unsigned char) value;
        buffer[1] = 0;
        int amt = write(fd, buffer, 1);
        close(fd);
        return amt == -1 ? -errno : 0;
    } else {
        if (already_warned == 0) {
            ALOGE("write_int failed to open %s\n", path);
            already_warned = 1;
        }
        return -errno;
    }
}

static int rgb_to_brightness(struct light_state_t const* state){
    int color = state->color & 0x00ffffff;
    return ((77*((color>>16)&0x00ff))
            + (150*((color>>8)&0x00ff)) + (29*(color&0x00ff))) >> 8;
}

static int set_light_backlight(struct light_device_t* dev,
        struct light_state_t const* state){
    int err = 0;
    int brightness = rgb_to_brightness(state);
    unsigned char new_brightness = (unsigned char)(brightness/14);

    ALOGD("Setting brightness: %d", brightness);
    
    pthread_mutex_lock(&g_lock);
    if (new_brightness != lastHID){
        //TODO: This is not very elegant....
        if (hid_fd < 0) hid_fd = openhid("/dev/hidraw0");
        if (hid_fd < 0) hid_fd = openhid("/dev/hidraw1");
        if (hid_fd < 0) hid_fd = openhid("/dev/hidraw2");
        if (hid_fd < 0) hid_fd = openhid("/dev/hidraw3");
        if (hid_fd < 0) hid_fd = openhid("/dev/hidraw4");
        if (hid_fd < 0) hid_fd = openhid("/dev/hidraw5");
        if (hid_fd >= 0) writehid(new_brightness);
        lastHID = new_brightness;
    }
    err = write_int(LCD_FILE, brightness);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int close_lights(struct light_device_t *dev){
    if (dev) {
        free(dev);
    }
    return 0;
}

/** Open a new instance of a lights device using name */
static int open_lights(const struct hw_module_t* module, char const* name,
        struct hw_device_t** device){
    int (*set_light)(struct light_device_t* dev,
            struct light_state_t const* state);

    if (0 == strcmp(LIGHT_ID_BACKLIGHT, name))
        set_light = set_light_backlight;
    else
        return -EINVAL;

    pthread_once(&g_init, init_globals);

    struct light_device_t *dev = malloc(sizeof(struct light_device_t));
    memset(dev, 0, sizeof(*dev));

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (struct hw_module_t*)module;
    dev->common.close = (int (*)(struct hw_device_t*))close_lights;
    dev->set_light = set_light;

    *device = (struct hw_device_t*)dev;
    return 0;
}

static struct hw_module_methods_t lights_module_methods = {
    .open =  open_lights,
};

struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .version_major = 1,
    .version_minor = 0,
    .id = LIGHTS_HARDWARE_MODULE_ID,
    .name = "lights Module",
    .author = "Adam Serbinski",
    .methods = &lights_module_methods,
};
