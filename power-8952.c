/*
 * Copyright (c) 2015,2018 The Linux Foundation. All rights reserved.
 * Copyright (C) 2018 The LineageOS Project
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * *    * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_NIDEBUG 0

#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#define LOG_TAG "QCOM PowerHAL"
#include <log/log.h>
#include <hardware/hardware.h>
#include <hardware/power.h>

#include "utils.h"
#include "metadata-defs.h"
#include "hint-data.h"
#include "performance.h"
#include "power-common.h"

#define MIN_VAL(X,Y) ((X>Y)?(Y):(X))

static int video_encode_hint_sent;
pthread_mutex_t camera_hint_mutex = PTHREAD_MUTEX_INITIALIZER;
static int camera_hint_ref_count;
static int display_fd;
#define SYS_DISPLAY_PWR "/sys/kernel/hbtp/display_pwr"

static int current_power_profile = PROFILE_BALANCED;

static int profile_high_performance[] = {
    CPUS_ONLINE_MIN_BIG, 0x4,
    CPUS_ONLINE_MIN_LITTLE, 0x4,
    CPU0_MIN_FREQ_TURBO_MAX,
    CPU4_MIN_FREQ_TURBO_MAX,
};

static int profile_power_save[] = {
    CPUS_ONLINE_MAX_BIG, 0x0,
    CPU0_MAX_FREQ_NONTURBO_MAX,
    CPU4_MAX_FREQ_NONTURBO_MAX,
};

static int profile_bias_power[] = {
    CPU0_MAX_FREQ_NONTURBO_MAX,
    CPU4_MAX_FREQ_NONTURBO_MAX,
};

static int profile_bias_performance[] = {
    CPU0_MIN_FREQ_NONTURBO_MAX + 1,
    CPU4_MIN_FREQ_NONTURBO_MAX + 1,
};

#ifdef INTERACTION_BOOST
int get_number_of_profiles()
{
    return 5;
}
#endif

static int set_power_profile(int profile)
{
    int ret = -EINVAL;
    const char *profile_name = NULL;

    if (profile == current_power_profile)
        return 0;

    ALOGV("%s: Profile=%d", __func__, profile);

    if (current_power_profile != PROFILE_BALANCED) {
        undo_hint_action(DEFAULT_PROFILE_HINT_ID);
        ALOGV("%s: Hint undone", __func__);
        current_power_profile = PROFILE_BALANCED;
    }

    if (profile == PROFILE_POWER_SAVE) {
        ret = perform_hint_action(DEFAULT_PROFILE_HINT_ID, profile_power_save,
                ARRAY_SIZE(profile_power_save));
        profile_name = "powersave";

    } else if (profile == PROFILE_HIGH_PERFORMANCE) {
        ret = perform_hint_action(DEFAULT_PROFILE_HINT_ID,
                profile_high_performance, ARRAY_SIZE(profile_high_performance));
        profile_name = "performance";

    } else if (profile == PROFILE_BIAS_POWER) {
        ret = perform_hint_action(DEFAULT_PROFILE_HINT_ID, profile_bias_power,
                ARRAY_SIZE(profile_bias_power));
        profile_name = "bias power";

    } else if (profile == PROFILE_BIAS_PERFORMANCE) {
        ret = perform_hint_action(DEFAULT_PROFILE_HINT_ID,
                profile_bias_performance, ARRAY_SIZE(profile_bias_performance));
        profile_name = "bias perf";
    } else if (profile == PROFILE_BALANCED) {
        ret = 0;
        profile_name = "balanced";
    }

    if (ret == 0) {
        current_power_profile = profile;
        ALOGD("%s: Set %s mode", __func__, profile_name);
    }
    return ret;
}

static bool is_target_SDM439() /* Returns value=1 if target is Hathi else value 0 */
{
    int fd;
    bool is_target_SDM439 = false;
    char buf[10] = {0};
    fd = open("/sys/devices/soc0/soc_id", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, buf, sizeof(buf) - 1) == -1) {
            ALOGW("Unable to read soc_id");
            is_target_SDM439 = false;
        } else {
            int soc_id = atoi(buf);
            if (soc_id == 353 || soc_id == 363 || soc_id == 354 || soc_id == 364) {
                is_target_SDM439 = true; /* Above SOCID for SDM439/429 */
            }
        }
    }
    close(fd);
    return is_target_SDM439;
}

static void process_video_encode_hint(void *metadata)
{
    char governor[80] = {0};
    int resource_values[20] = {0};
    int num_resources = 0;
    struct video_encode_metadata_t video_encode_metadata;

    if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU0) == -1) {
        if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU1) == -1) {
            if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU2) == -1) {
                if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU3) == -1) {
                    ALOGE("Can't obtain scaling governor.");
                    return;
                }
            }
        }
    }

    if (!metadata) {
        return;
    }

    /* Initialize encode metadata struct fields. */
    memset(&video_encode_metadata, 0, sizeof(struct video_encode_metadata_t));
    video_encode_metadata.state = -1;
    video_encode_metadata.hint_id = DEFAULT_VIDEO_ENCODE_HINT_ID;

    if (parse_video_encode_metadata((char *)metadata,
            &video_encode_metadata) == -1) {
        ALOGE("Error occurred while parsing metadata.");
        return;
    }

    if (video_encode_metadata.state == 1) {
        if((strncmp(governor, SCHEDUTIL_GOVERNOR,
            strlen(SCHEDUTIL_GOVERNOR)) == 0) &&
            (strlen(governor) == strlen(SCHEDUTIL_GOVERNOR))) {
            if(is_target_SDM439()) {
                /* sample_ms = 10mS
                * SLB for Core0 = -6
                * SLB for Core1 = -6
                * SLB for Core2 = -6
                * SLB for Core3 = -6
                * hispeed load = 95
                * hispeed freq = 998Mhz */
                int res[] = {0x41820000, 0xa,
                             0x40c68100, 0xfffffffa,
                             0x40c68110, 0xfffffffa,
                             0x40c68120, 0xfffffffa,
                             0x40c68130, 0xfffffffa,
                             0x41440100, 0x5f,
                             0x4143c100, 0x3e6,
                             };
                memcpy(resource_values, res, MIN_VAL(sizeof(resource_values), sizeof(res)));
                num_resources = sizeof(res)/sizeof(res[0]);
                pthread_mutex_lock(&camera_hint_mutex);
                camera_hint_ref_count++;
                if (camera_hint_ref_count == 1) {
                    if (!video_encode_hint_sent) {
                        perform_hint_action(video_encode_metadata.hint_id,
                        resource_values, num_resources);
                        video_encode_hint_sent = 1;
                    }
                }
                pthread_mutex_unlock(&camera_hint_mutex);
            }
            else {
                /* sample_ms = 10mS */
                int res[] = {0x41820000, 0xa,
                            };
                memcpy(resource_values, res, MIN_VAL(sizeof(resource_values), sizeof(res)));
                num_resources = sizeof(res)/sizeof(res[0]);
                pthread_mutex_lock(&camera_hint_mutex);
                camera_hint_ref_count++;
                if (camera_hint_ref_count == 1) {
                    if (!video_encode_hint_sent) {
                        perform_hint_action(video_encode_metadata.hint_id,
                        resource_values, num_resources);
                        video_encode_hint_sent = 1;
                    }
                }
                pthread_mutex_unlock(&camera_hint_mutex);
            }
        } else if (is_interactive_governor(governor)) {
            int res[] = {
                INT_OP_CLUSTER0_USE_SCHED_LOAD, 0x1,
                INT_OP_CLUSTER1_USE_SCHED_LOAD, 0x1,
                INT_OP_CLUSTER0_USE_MIGRATION_NOTIF, 0x1,
                INT_OP_CLUSTER1_USE_MIGRATION_NOTIF, 0x1,
                INT_OP_CLUSTER0_TIMER_RATE, BIG_LITTLE_TR_MS_40,
                INT_OP_CLUSTER1_TIMER_RATE, BIG_LITTLE_TR_MS_40
            };
            memcpy(resource_values, res, MIN_VAL(sizeof(resource_values), sizeof(res)));
            num_resources = sizeof(res)/sizeof(res[0]);
            pthread_mutex_lock(&camera_hint_mutex);
            camera_hint_ref_count++;
            if (!video_encode_hint_sent) {
                perform_hint_action(video_encode_metadata.hint_id,
                        resource_values, num_resources);
                video_encode_hint_sent = 1;
            }
            pthread_mutex_unlock(&camera_hint_mutex);
        }
    } else if (video_encode_metadata.state == 0) {
        if (is_interactive_governor(governor) ||
            ((strncmp(governor, SCHEDUTIL_GOVERNOR,
            strlen(SCHEDUTIL_GOVERNOR)) == 0) &&
            (strlen(governor) == strlen(SCHEDUTIL_GOVERNOR)))) {
            pthread_mutex_lock(&camera_hint_mutex);
            camera_hint_ref_count--;
            if (!camera_hint_ref_count) {
                undo_hint_action(video_encode_metadata.hint_id);
                video_encode_hint_sent = 0;
            }
            pthread_mutex_unlock(&camera_hint_mutex);
        }
    }
}

int power_hint_override(power_hint_t hint, void *data)
{
    if (hint == POWER_HINT_SET_PROFILE) {
        if (set_power_profile(*(int32_t *)data) < 0)
            ALOGE("Setting power profile failed. perfd not started?");
        return HINT_HANDLED;
    }

    // Skip other hints in high/low power modes
    if (current_power_profile == PROFILE_POWER_SAVE ||
            current_power_profile == PROFILE_HIGH_PERFORMANCE) {
        return HINT_HANDLED;
    }

    switch (hint) {
        case POWER_HINT_VIDEO_ENCODE:
            process_video_encode_hint(data);
            return HINT_HANDLED;
        default:
            break;
    }
    return HINT_NONE;
}

int set_interactive_override(int on)
{
    char governor[80];
    int rc = 0;

    static const char *display_on = "1";
    static const char *display_off = "0";
    char err_buf[80];
    static int init_interactive_hint = 0;
    static int set_i_count = 0;

    if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU0) == -1) {
        if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU1) == -1) {
            if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU2) == -1) {
                if (get_scaling_governor_check_cores(governor, sizeof(governor), CPU3) == -1) {
                    ALOGE("Can't obtain scaling governor.");
                    return HINT_NONE;
                }
            }
        }
    }

    if (!on) {
        /* Display off. */
        if (is_interactive_governor(governor)) {
            int resource_values[] = {
                INT_OP_CLUSTER0_TIMER_RATE, BIG_LITTLE_TR_MS_50,
                INT_OP_CLUSTER1_TIMER_RATE, BIG_LITTLE_TR_MS_50,
                INT_OP_NOTIFY_ON_MIGRATE, 0x00
            };
            perform_hint_action(DISPLAY_STATE_HINT_ID,
                    resource_values, ARRAY_SIZE(resource_values));
        }
    } else {
        /* Display on. */
        if (is_interactive_governor(governor)) {
            undo_hint_action(DISPLAY_STATE_HINT_ID);
        }
    }

    set_i_count ++;
    ALOGI("Got set_interactive hint on= %d, count= %d\n", on, set_i_count);

    if (init_interactive_hint == 0)
    {
        //First time the display is turned off
        display_fd = TEMP_FAILURE_RETRY(open(SYS_DISPLAY_PWR, O_RDWR));
        if (display_fd < 0) {
            strerror_r(errno,err_buf,sizeof(err_buf));
            ALOGE("Error opening %s: %s\n", SYS_DISPLAY_PWR, err_buf);
            return HINT_HANDLED;
        }
        else
            init_interactive_hint = 1;
    }
    else
        if (!on ) {
            /* Display off. */
            rc = TEMP_FAILURE_RETRY(write(display_fd, display_off, strlen(display_off)));
            if (rc < 0) {
                strerror_r(errno,err_buf,sizeof(err_buf));
                ALOGE("Error writing %s to  %s: %s\n", display_off, SYS_DISPLAY_PWR, err_buf);
            }
        }
        else {
            /* Display on */
            rc = TEMP_FAILURE_RETRY(write(display_fd, display_on, strlen(display_on)));
            if (rc < 0) {
                strerror_r(errno,err_buf,sizeof(err_buf));
                ALOGE("Error writing %s to  %s: %s\n", display_on, SYS_DISPLAY_PWR, err_buf);
            }
        }

    return HINT_HANDLED;
}
