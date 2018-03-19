#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mount.h>
#include <sys/system_properties.h>
#include <cutils/properties.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <android/log.h>

int skipsleep;

int revalphasort(const struct dirent **pa, const struct dirent **pb){
	return alphasort(pb, pa);
}

int compar(const struct dirent **pa, const struct dirent **pb){
	const char *a = (*pa)->d_name;
	const char *b = (*pb)->d_name;
	struct stat ainfo, binfo;
	char fulla[256], fullb[256];
	sprintf(fulla, "/oem/%s", a);
	sprintf(fullb, "/oem/%s", b);
	if (stat(fulla, &ainfo) != 0 || stat(fullb, &binfo) != 0) return 0;
	if (ainfo.st_mtime == binfo.st_mtime) return 0;
	else if (ainfo.st_mtime < binfo.st_mtime) return 1;
	return -1;
}

int filter(const struct dirent *d){
        struct dirent a = *d;
        struct stat ainfo;
	char fullpath[256];
	sprintf(fullpath, "/oem/%s", a.d_name);
        stat(fullpath, &ainfo);
        if(S_ISREG(ainfo.st_mode)) return 1;
        return 0;
}

int checkfree(){
        struct statvfs stat;
        float pfree;
        if (statvfs("/oem", &stat) != 0) return -1;
        pfree = ((float)stat.f_bfree / (float)stat.f_blocks) * 100.0;
	__android_log_print(ANDROID_LOG_DEBUG, "CAMd", "Free space: %f%% (float), %d%% (int)\n", pfree, (int)pfree);
        return (int)pfree;
}

void *reap(){
	struct dirent **namelist;
	int n, a;
	char fullpath[256];
	while (1){
		a = 1;
		if (checkfree() < (100 - 90)){
			n = scandir("/oem", &namelist, *filter, *compar);
			if (n < 0) __android_log_print(ANDROID_LOG_ERROR, "CAMd", "reaper scandir error: %s", strerror(errno));
			else {
				__android_log_print(ANDROID_LOG_DEBUG, "CAMd", "Reaper running, scanned %d files", n);
				while (n > 0) {
					n--;
					if (a && checkfree() < (100 - 90)){
						sprintf(fullpath, "/oem/%s", namelist[n]->d_name);
						__android_log_print(ANDROID_LOG_DEBUG, "CAMd", "Reaping file: %s", fullpath);
						if (unlink(fullpath) < 0)
							__android_log_print(ANDROID_LOG_ERROR, "CAMd", "Unlink error: %s", strerror(errno));
					} else a=0;
					free(namelist[n]);
				}
				free(namelist);
			}
		}
		sleep (5*60); // sleep for 5 minutes
	}
}

void *timewatch(){
	time_t last, current;
	last = time(NULL);
	while (1){
		current = time(NULL);
		sleep(10);
		if ((current - last) > 60 || (current - last) < -50){ // if the time jumped too far
			__android_log_print(ANDROID_LOG_DEBUG, "CAMd", "Excessive time jump detected, resetting ffmpeg.");
			skipsleep = 1;
			system("/system/bin/killall -9 ffmpeg"); // ffmpeg will be immediately restarted
		}
		last = current;
	}
}

int main(){
	struct dirent **namelist;
	int i, n, streams;
	ssize_t target_len;
	char target[1024];
	char fullpath[256];

	char dashcam_en[PROP_VALUE_MAX];
	char front_path[PROP_VALUE_MAX];
	char front_params[PROP_VALUE_MAX];
	char front_sub[PROP_VALUE_MAX];
	char rear_path[PROP_VALUE_MAX];
	char rear_params[PROP_VALUE_MAX];
	char rear_sub[PROP_VALUE_MAX];
	char left_path[PROP_VALUE_MAX];
	char left_params[PROP_VALUE_MAX];
	char left_sub[PROP_VALUE_MAX];
	char right_path[PROP_VALUE_MAX];
	char right_params[PROP_VALUE_MAX];
	char right_sub[PROP_VALUE_MAX];
	char audio_path[PROP_VALUE_MAX];

	char front_dev[32];
	char rear_dev[32];
	char left_dev[32];
	char right_dev[32];
	char audio_dev[32];

	int fsubc, bsubc, lsubc, rsubc;

	pthread_t reaper_thread, timewatch_thread;
	
	property_get("persist.dashcam.enabled", dashcam_en, "0");
	if (atoi(dashcam_en) == 0){
		__android_log_print(ANDROID_LOG_ERROR, "CAMd", "Dashcam is DISABLED, exiting.");
		exit(1); // dashcam recording is disabled, so terminate now.
	}
	__android_log_print(ANDROID_LOG_DEBUG, "CAMd", "Dashcam starting up.");
	umask(0022);
	/*
	 * Specific cameras will be identified by their USB path, which will remain constant
	 * as long as the physical connection heirarchy remains constant.
	 *
	 * Video devices will be listed in /sys/class/video4linux/ as symlinks as;
	 * video0 -> ../../devices/platform/soc/ff200000.hisi_usb/ff100000.dwc3/xhci-hcd.0.auto/usb1/1-1/1-1.2/1-1.2.2/1-1.2.2:1.0/video4linux/video0
	 *
	 * Property persist.dashcam.*.path should reflect the most relevant subpath of that, including
	 * leading and trailing '/', in this case;
	 * /1-1.2.2:1.0/
	 *
	 * Property persist.dashcam.*.params will involve settings for selected encoding, resolution, framerate, and other
	 * parameters for which ffmpeg is able to configure v4l2. For example:
	 *
	 * 1280x720 h264;
	 * -input_format h264 -video_size 1280x720
	 *
	 * 1280x720 mjpeg 5 fps;
	 * -r 5 -input_format mjpeg -video_size 1280x720
	 *
	 * *** NOTE: best to choose a lower framerate for mjpeg or it will consume enormous amounts of space, on the order ot 200 MB/min at 1280x720.
	 * mjpeg scales linearly with framerate.
	 *
	 * *** NOTE: When using multiple cameras, best if only the first stream is h264, since the i-frames will not align between multiple streams.
	 * The effect is that secondary h264 streams will have several broken frames at the beginning of each segment until the first I-frame arrives.
	 *
	 * Property persist.dashcam.*.sub will allow selection between multiple v4l2 devices presented by a single real device for cameras that may
	 * present multiple. For instance, some cameras will have /dev/videoX for raw+mjpeg, and /dev/videoX+1 for h264. In this case, to select the
	 * h264 device, persist.dashcam.*.sub=1
	 */ 

	property_get("persist.dashcam.front.path", front_path, "");
	property_get("persist.dashcam.front.params", front_params, "");
	property_get("persist.dashcam.front.sub", front_sub, "0");
	property_get("persist.dashcam.rear.path", rear_path, "");
	property_get("persist.dashcam.rear.params", rear_params, "");
	property_get("persist.dashcam.rear.sub", rear_sub, "0");
	property_get("persist.dashcam.left.path", left_path, "");
	property_get("persist.dashcam.left.params", left_params, "");
	property_get("persist.dashcam.left.sub", left_sub, "0");
	property_get("persist.dashcam.right.path", right_path, "");
	property_get("persist.dashcam.right.params", right_params, "");
	property_get("persist.dashcam.right.sub", right_sub, "0");
	property_get("persist.dashcam.audio.path", audio_path, "");

	if (access("/oem", W_OK) < 0){
		__android_log_print(ANDROID_LOG_ERROR, "CAMd", "Storage path /oem is not writable: %s", strerror(errno));
		exit(1);
	}

	if (strlen(front_path) > 0){
		pthread_create(&reaper_thread, NULL, reap, NULL);
		pthread_create(&timewatch_thread, NULL, timewatch, NULL);
	}

	while (1){
		streams = 1;
		fsubc=0; bsubc=0; lsubc=0; rsubc=0;
		front_dev[0] = '\0'; rear_dev[0] = '\0'; left_dev[0] = '\0'; right_dev[0] = '\0';
		target[0] = '\0';
		skipsleep = 0;

		n = scandir("/sys/class/video4linux", &namelist, NULL, revalphasort);
		if (n < 0) __android_log_print(ANDROID_LOG_ERROR, "CAMd", "SCANDIR error");
		else while (n--){
			sprintf(fullpath, "/sys/class/video4linux/%s", namelist[n]->d_name);
			target_len = readlink(fullpath, target, sizeof(target));
			if (target_len < 0) __android_log_print(ANDROID_LOG_ERROR, "CAMd", "readlink error: %s", strerror(errno));
			target[target_len] = '\0';

			__android_log_print(ANDROID_LOG_DEBUG, "CAMd", "Link (%zd): %s -> %s", target_len, namelist[n]->d_name, target);

			if (strlen(front_path) > 1 && strstr(target, front_path) != NULL){
				if (fsubc == atoi(front_sub)) strcpy(front_dev, namelist[n]->d_name);
				fsubc++;
			}

			if (strlen(rear_path) > 1 && strstr(target, rear_path) != NULL){
				if (bsubc == atoi(rear_sub)) strcpy(rear_dev, namelist[n]->d_name);
				bsubc++;
			}

			if (strlen(left_path) > 1 && strstr(target, left_path) != NULL){
				if (lsubc == atoi(left_sub)) strcpy(left_dev, namelist[n]->d_name);
				lsubc++;
			}

			if (strlen(right_path) > 1 && strstr(target, right_path) != NULL){
				if (rsubc == atoi(right_sub)) strcpy(right_dev, namelist[n]->d_name);
				rsubc++;
			}
			free(namelist[n]);
		}
		free(namelist);

		audio_dev[0] = '\0';
		if (strlen(audio_path) > 0){
			n = scandir("/sys/class/sound", &namelist, NULL, NULL);
			if (n < 0) __android_log_print(ANDROID_LOG_ERROR, "CAMd", "SCANDIR error");
			else while (n--){
				sprintf(fullpath, "/sys/class/sound/%s", namelist[n]->d_name);
				target_len = readlink(fullpath, target, sizeof(target));
				if (target_len < 0) __android_log_print(ANDROID_LOG_ERROR, "CAMd", "readlink (audio) error: %s", strerror(errno));
				target[target_len] = '\0';
				__android_log_print(ANDROID_LOG_DEBUG, "CAMd", "Link (%zd): %s -> %s", target_len, namelist[n]->d_name, target);
				if (strstr(namelist[n]->d_name, "dsp") != NULL && strstr(target, audio_path) != NULL){
					strcpy(audio_dev, namelist[n]->d_name);
					break;
				}
				free(namelist[n]);
			}
			free(namelist);
		}

		__android_log_print(ANDROID_LOG_DEBUG, "CAMd", "Device files computed. Generating ffmpeg commandline...");
		// If any of the devices that are supposed to be present are not yet, skip execution for this iteration.
		if (strlen(front_dev) > 0 && !((strlen(rear_path) > 0 && strlen(rear_dev) == 0) ||
			(strlen(left_path) > 0 && strlen(left_dev) == 0) ||
			(strlen(right_path) > 0 && strlen(right_dev) == 0) ||
			(strlen(audio_path) > 0 && strlen(audio_dev) == 0))){

			// Generate ffmpeg command line:
			sprintf(target, "/system/bin/logwrapper /system/bin/ffmpeg -f v4l2 -thread_queue_size 512 %s -i /dev/%s", front_params, front_dev);
			if (strlen(rear_dev) > 0){
				sprintf(&target[strlen(target)], " -f v4l2 -thread_queue_size 512 %s -i /dev/%s", rear_params, rear_dev);
				streams++;
			}
			if (strlen(left_dev) > 0){
				sprintf(&target[strlen(target)], " -f v4l2 -thread_queue_size 512 %s -i /dev/%s", left_params, left_dev);
				streams++;
			}
			if (strlen(right_dev) > 0){
				sprintf(&target[strlen(target)], " -f v4l2 -thread_queue_size 512 %s -i /dev/%s", right_params, right_dev);
				streams++;
			}
			if (strlen(audio_dev) > 0){
				sprintf(&target[strlen(target)], " -f oss -thread_queue_size 512 -i /dev/snd/%s -ac 1 -af aresample=async=1 -c:a mp2 -b:a 32000 ", audio_dev);
				streams++;
			}
			sprintf(&target[strlen(target)], " -c:v copy");
			for (i = 0; i < streams; i++){
				sprintf(&target[strlen(target)], " -map %d", i);
			}
			sprintf(&target[strlen(target)], " -copyts -start_at_zero -copytb 1 -f segment -strftime 1 -segment_time 60 -segment_atclocktime 1 -reset_timestamps 1 /oem/cam_%%Y-%%m-%%d_%%H-%%M-%%S_UTC.mkv");

			__android_log_print(ANDROID_LOG_DEBUG, "CAMd", "Executing: %s", target);

			system(target);
		} else
			__android_log_print(ANDROID_LOG_ERROR, "CAMd", "Unable to find all configured devices. Trying again in 10 seconds.");

		if (!skipsleep) sleep(10);
	}
}

