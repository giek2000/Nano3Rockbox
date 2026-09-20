/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * iPod Nano 3G ("N46") audio source selection glue.
 *
 * Original implementation for this project. This layer only decides
 * which audio path is active (playback vs. recording sources) and
 * delegates the actual codec configuration to audiohw_* calls implemented
 * in wmcodec-nano3g.c; there is no target-specific hardware knowledge in
 * this file.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "system.h"
#include "cpu.h"
#include "audio.h"
#include "sound.h"

#if INPUT_SRC_CAPS != 0

void audio_set_output_source(int source)
{
    if ((unsigned)source >= AUDIO_NUM_SOURCES)
        source = AUDIO_SRC_PLAYBACK;
}

void audio_input_mux(int source, unsigned flags)
{
    static int active_source = AUDIO_SRC_PLAYBACK;
    (void)flags;

    switch (source)
    {
        default:
            source = AUDIO_SRC_PLAYBACK;
            /* fall through */
        case AUDIO_SRC_PLAYBACK:
#ifdef HAVE_RECORDING
            if (source != active_source)
            {
                audiohw_set_monitor(false);
                audiohw_disable_recording();
            }
#endif
            break;

#ifdef HAVE_MIC_REC
        case AUDIO_SRC_MIC:
            if (source != active_source)
            {
                audiohw_set_monitor(false);
                audiohw_enable_recording(true);  /* source: mic */
            }
            break;
#endif

#ifdef HAVE_LINE_REC
        case AUDIO_SRC_LINEIN:
            if (source != active_source)
            {
                audiohw_set_monitor(false);
                audiohw_enable_recording(false); /* source: line */
            }
            break;
#endif
    }

    active_source = source;
}

#endif /* INPUT_SRC_CAPS != 0 */
