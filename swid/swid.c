#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <android/log.h>
#include <linux/input.h>
#include <linux/uinput.h>

#define DOWN 1
#define UP 0

int serial_fd;
int key_fd;
int key_client_fd;
int bl_fd;
int bl_client_fd;
int uinput_fd;
struct uinput_user_dev uidev;

void emit(int fd, int type, int code, int val)
{
   struct input_event ie;

   ie.type = type;
   ie.code = code;
   ie.value = val;
   /* timestamp values below are ignored */
   ie.time.tv_sec = 0;
   ie.time.tv_usec = 0;

   write(fd, &ie, sizeof(ie));
}

int uinput_init() {
	int i;
	uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if(uinput_fd < 0) return -1;
	if(ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY) < 0) return -2;
	// enable ALL keys...
	for (i=0; i<0xff; i++){
		if(ioctl(uinput_fd, UI_SET_KEYBIT, i) < 0) return -4;
	}

#if !defined(UI_SET_PROPBIT)
#define UI_SET_PROPBIT    _IOW(UINPUT_IOCTL_BASE, 110, int)
#endif
	ioctl(uinput_fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
	memset(&uidev, 0, sizeof(uidev));
	snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "uinput-droid");
	uidev.id.bustype = BUS_VIRTUAL;
	uidev.id.vendor  = 0x1;
	uidev.id.product = 0x1;
	uidev.id.version = 1;
	if(write(uinput_fd, &uidev, sizeof(uidev)) < 0) return -5;
	if(ioctl(uinput_fd, UI_DEV_CREATE) < 0) return -6;
	sleep(2);

	return 0;
}

// I don't think we need this since it should keep running forever.
//void uinput_term(j) {
//	if(ioctl(uinput_fd, UI_DEV_DESTROY) >= 0) close(uinput_fd);
//}

void uinput_keyevt(char key, int direction) {
	struct input_event ev;

	memset(&ev, 0, sizeof(struct input_event));
	ev.type = EV_KEY;
	ev.code = key;
	ev.value = direction;
	if(write(uinput_fd, &ev, sizeof(struct input_event)) < 0) return;

	memset(&ev, 0, sizeof(struct input_event));
	ev.type = EV_SYN;
	ev.code = 0;
	ev.value = 0;
	if(write(uinput_fd, &ev, sizeof(struct input_event)) < 0) return;
}

int set_interface_attribs (int fd, int speed, int parity){
	struct termios tty;
	memset (&tty, 0, sizeof tty);
	if (tcgetattr (fd, &tty) != 0){
		printf ("error %d from tcgetattr\n", errno);
		return -1;
	}

	cfsetospeed (&tty, speed);
	cfsetispeed (&tty, speed);

	tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;	// 8-bit chars
	// disable IGNBRK for mismatched speed tests; otherwise receive break as \000 chars
	tty.c_iflag &= ~IGNBRK;				// disable break processing
	tty.c_lflag = 0;				// no signaling chars, no echo,
							// no canonical processing
	tty.c_oflag = 0;				// no remapping, no delays
	tty.c_cc[VMIN]  = 0xff;				// read blocks for min 255 bytes
	tty.c_cc[VTIME] = 5;				// 0.5 seconds read timeout

	tty.c_iflag &= ~(IXON | IXOFF | IXANY);		// shut off xon/xoff ctrl

	tty.c_cflag |= (CLOCAL | CREAD);		// ignore modem controls,
							// enable reading
	tty.c_cflag &= ~(PARENB | PARODD);		// shut off parity
	tty.c_cflag |= parity;
	tty.c_cflag &= ~CSTOPB;
	tty.c_cflag &= ~CRTSCTS;

	if (tcsetattr (fd, TCSANOW, &tty) != 0){
		printf ("error %d from tcsetattr\n", errno);
		return -1;
	}
	return 0;
}

int create_socket(char *path){
	int fd;
	struct sockaddr_un sun;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	if (fcntl(fd, F_SETFD, FD_CLOEXEC)) {
		close(fd);
		return -1;
	}

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_LOCAL;
	memset(sun.sun_path, 0, sizeof(sun.sun_path));
	snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", path);

	unlink(sun.sun_path);

	if (bind(fd, (struct sockaddr*)&sun, sizeof(sun)) < 0 || listen(fd, 1) < 0){
		close(fd);
		return -1;
	}

	return fd;
}

void *key_read(){
	int rc;
	char buf[3];
	// forever listen for connections
	while (1){
		if ((key_client_fd = accept(key_fd, NULL, NULL)) == -1) {
			__android_log_print(ANDROID_LOG_ERROR, "SWId", "Key accept error");
			continue;
		}

		// forever read the client until the client disconnects.
		while (1){
			rc = read(key_client_fd, buf, 1);
			if (rc <= 0) break;
			__android_log_print(ANDROID_LOG_DEBUG, "SWId", "Key read byte: %02X", buf[0]);
			buf[1] = '\n';
			buf[2] = 0;
			write(serial_fd, buf, 2);
		}
		if (rc == -1) {
			__android_log_print(ANDROID_LOG_DEBUG, "SWId", "Key read error");
			close(key_client_fd);
		} else if (rc == 0) {
			__android_log_print(ANDROID_LOG_DEBUG, "SWId", "Key EOF");
			close(key_client_fd);
		}
		key_client_fd = 0;
	}
	return 0;
}

void *bl_read(){
        int rc;
        char buf[4];
	buf[0] = 'B';
        // forever listen for connections
        while (1){
                if ((bl_client_fd = accept(bl_fd, NULL, NULL)) == -1) {
                        __android_log_print(ANDROID_LOG_ERROR, "SWId", "Backlight accept error");
                        continue;
                }

                // forever read the client until the client disconnects.
                while (1){
                        rc = read(bl_client_fd, buf+1, 1);
                        if (rc <= 0) break;
			__android_log_print(ANDROID_LOG_DEBUG, "SWId", "Backlight Read byte: %02X", buf[1]);
                        buf[2] = '\n';
                        buf[3] = 0;
                        write(serial_fd, buf, 3);
                }
                if (rc == -1) {
                        __android_log_print(ANDROID_LOG_DEBUG, "SWId", "Backlight read error");
                        close(bl_client_fd);
                } else if (rc == 0) {
                        __android_log_print(ANDROID_LOG_DEBUG, "SWId", "Backlight EOF");
                        close(bl_client_fd);
                }
                bl_client_fd = 0;
        }
        return 0;
}

void *temp_read(){

	/*
	 * Read from /sys/devices/virtual/thermal/thermal_zone0/temp
	 */

	int lasttemp = 0, curtemp;
	unsigned char lastpwm = 0x00;
	unsigned char curpwm = 0x00;
	char buff[10];
        char wbuf[4];
        wbuf[0] = 'T';

	int tempfd = open("/sys/devices/virtual/thermal/thermal_zone0/temp", O_RDONLY);
	if (tempfd <= 0) return 0;
	__android_log_print(ANDROID_LOG_DEBUG, "SWId", "Entered thermal monitoring loop");
	while (1){
		lseek(tempfd, 0, SEEK_SET);
		read(tempfd, buff, 6);
		curtemp = atoi(buff);
		__android_log_print(ANDROID_LOG_DEBUG, "SWId", "Read temperature: %d", curtemp);
		if (curtemp > lasttemp){
			// temperature is increasing
			if (curtemp >= 65000) curpwm = 0xff; // 100%
			else if (curtemp >= 60000) curpwm = 0xe5; // 90%
			else if (curtemp >= 55000) curpwm = 0xcc; // 80%
			else if (curtemp >= 50000) curpwm = 0xb2; // 70%
			else if (curtemp >= 45000) curpwm = 0x99; // 60%
			else if (curtemp >= 40000) curpwm = 0x7f; // 50%
		} else {
			// temperature is decreasing
			if (curtemp < 35000) curpwm = 0x00; // 0%
			else if (curtemp < 40000) curpwm = 0x7f; // 50%
			else if (curtemp < 45000) curpwm = 0x99; // 60%
			else if (curtemp < 50000) curpwm = 0xb2; // 70%
			else if (curtemp < 55000) curpwm = 0xcc; // 80%
			else if (curtemp < 60000) curpwm = 0xe5; // 90%
		}
		if (curpwm != lastpwm){
			__android_log_print(ANDROID_LOG_DEBUG, "SWId", "Sending fan update: %02X", curpwm);
			wbuf[1] = curpwm;
			wbuf[2] = '\n';
			write(serial_fd, wbuf, 3);
			lastpwm = curpwm;
			lasttemp = curtemp;
		}
		sleep(1);
	}
	return 0;
}

int main(){
	char buf[64];
	int bufidx, i;
	char *cmd;
	char *serialportname = "/dev/ttyAMA3";

	pthread_t key_reader;
	pthread_t bl_reader;
	pthread_t temp_reader;

	//daemon(0,0);

	serial_fd = open (serialportname, O_RDWR | O_NOCTTY | O_SYNC);
	if (serial_fd < 0){
		__android_log_print(ANDROID_LOG_ERROR, "SWId", "error %d opening %s: %s\n", errno, serialportname, strerror (errno));
		return -1;
	}

	if (set_interface_attribs (serial_fd, B115200, 0) < 0) return -1;

	// Initialize uinput
	if (uinput_init() < 0) return -1;

	key_fd = create_socket("/data/vendor/swi");

	system ("/system/bin/chmod 644 /data/vendor/swi");
	system ("/system/bin/chown system.system /data/vendor/swi");
	system ("/system/bin/chcon u:object_r:swid_sock:s0 /data/vendor/swi");

	if (pthread_create(&key_reader, NULL, key_read, NULL) != 0) return -1;
	pthread_detach(key_reader);

	bl_fd = create_socket("/data/vendor/backlight");

	system ("/system/bin/chmod 644 /data/vendor/backlight");
	system ("/system/bin/chown system.system /data/vendor/backlight");
	system ("/system/bin/chcon u:object_r:swid_sock:s0 /data/vendor/backlight");


	if (pthread_create(&bl_reader, NULL, bl_read, NULL) != 0) return -1;
	pthread_detach(bl_reader);

	if (pthread_create(&temp_reader, NULL, temp_read, NULL) != 0) return -1;
	pthread_detach(temp_reader);

	while (1){
		bufidx = 0;
		while (bufidx < 64 && (i = read(serial_fd, &buf[bufidx], 1)) >= 0){
			if (buf[bufidx] == '\n'){ // if we read a \n, means we fully read the command
				buf[bufidx+1] = 0; // null terminate the string
				printf("%s", cmd);
				if ((cmd = strstr(buf, "KEYDOWN")) > 0){
					__android_log_print(ANDROID_LOG_VERBOSE, "SWId", "Sending uinput key event DOWN: %d", cmd[8]);

					// keycode 0x98 is "KEYCODE_COFFEE", normally mapped as an alternate "POWER" key. Use that as a REBOOT button.
					if (cmd[8] == 0x98) system("/system/bin/reboot");
					uinput_keyevt(cmd[8], DOWN);
				} else if ((cmd = strstr(buf, "KEYUP")) > 0){
					__android_log_print(ANDROID_LOG_VERBOSE, "SWId", "Sending uinput key event UP: %d\n", cmd[6]);
					uinput_keyevt(cmd[6], UP);
				} else if ((cmd = strstr(buf, "PINPUT")) > 0){
					write(key_fd, buf, strlen(buf));
					__android_log_print(ANDROID_LOG_VERBOSE, "SWId", "%s", cmd);
					// There are 6 (usable) analog inputs and 13 digital.
					// Each analog input takes 2 bytes.
					// Each digital input takes 1 bit.
					// Send the entire state in 14 bytes.
				} else if ((cmd = strstr(buf, "DEBUG")) > 0){
					__android_log_print(ANDROID_LOG_VERBOSE, "SWId", "%s", cmd);
				}

				// reset buffer and start reading next input.
				bufidx = 0;
				break;
			}
			bufidx += i; // increment only if a byte was actually read
		}
	}
}

