#include "Samples.h"
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
const char KVS_STREAMER_REV[] = "005";
/*********************************************************************************************
Revision History:
Rev.      By   Date      Change Description
--------  ---  --------  ---------------------------------------------------------------------
 005  DM   05/19/2020
 Action:
 a. Rename video file from cb_kvs to cb_vid.mp4
 
 004  DM   05/13/2020
 Problem:
 a. Viewer fail to receive video through cellular.
 Observation:
 a. MP4 file bit rate is 1.3Mbs
 Action:
 a. Change rpicamsrce bit rate from default 17Mbs to 1.3Mbs
 
 003  DM   05/07/2020
 Problem:
 a. Unable to play mp4 file because it is not closed properly.
 Observation:
 a. Pipeline need EOS (End Of Stream) signal to close mp4 file properly
    Fail to link when Common.c calling function defined in kvsWebRTCClientMasterGstreamerSample.c
 Action:
 a. Create send_eos(). assign send_eos() address to function pinter *fun_ptr.
    use *fun_ptr to call the function form Common.c
 b. Clean compiler warnings
 c. Uncomment THREAD_JOIN(pSampleConfiguration->videoSenderTid, NULL)
 d. Remove requirement for third parameter (i.e. ./kvsWebrtcClientMasterGstSample TEST_1918 is sufficient).
 e. store video file at /home/pi/Videos/cb_kvs.mp4

 002  DM   05/03/2020
 Action:
 a. Enable saving stream to file on startup add startSenderMediaThreads() in common.c.
 b. Change pipeline to enable saving to mp4 file (omxh264enc element is removed).
 c. Add gst_app_src_end_of_stream()
 Problem:
 b. EOS is not sent when we close the application. therefore the mp4 file is not closed properly.

	When you press ctrl+c the source element does not push an EOS event to the downstream elements,
	the pipeline is simply shut down. The muxer needs to receive an EOS event to know that the data
	streaming has ended and properly finish the mp4 file (like seeking back to the start of the file
	to rewrite some fields that he coudn't write back at the start).
	if you use '-e' option in gst-launch it forces an EOS at the pipeline when ctrl+c is pressed and all should work fine.
    http://gstreamer-devel.966125.n4.nabble.com/My-H-264-encoder-cound-not-work-well-with-the-MP4-muxer-td973490i20.html#a973502
 Action:
 b. Save the file into mkv format for now until we have solution for EOS issue

 001  DM   05/04/2020
 Action:
 a. Modify pipe line:
    Enable audio and video recording into mkv file 
	Use rpicamssrc element
	Use hardware accelerated video encoding omxh264enc element 
*********************************************************************************************/

extern PSampleConfiguration gSampleConfiguration;
GstElement *pipeline = NULL;/*global pipeline to support send_eos()*/

//#define VERBOSE

GstFlowReturn on_new_sample(GstElement *sink, gpointer data, UINT64 trackid)
{
    GstBuffer *buffer;
    BOOL isDroppable, delta;
    GstFlowReturn ret = GST_FLOW_OK;
    GstSample *sample = NULL;
    GstMapInfo info;
    GstSegment *segment;
    GstClockTime buf_pts;
    Frame frame;
    STATUS status;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) data;
    PSampleStreamingSession pSampleStreamingSession = NULL;
    PRtcRtpTransceiver pRtcRtpTransceiver = NULL;
    UINT32 i;

    if(pSampleConfiguration == NULL) {
        printf("[KVS GStreamer Master] on_new_sample(): operation returned status code: 0x%08x \n", STATUS_NULL_ARG);
        goto CleanUp;
    }

    info.data = NULL;
    sample = gst_app_sink_pull_sample(GST_APP_SINK (sink));

    buffer = gst_sample_get_buffer(sample);
    isDroppable = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_CORRUPTED) ||
                  GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DECODE_ONLY) ||
                  (GST_BUFFER_FLAGS(buffer) == GST_BUFFER_FLAG_DISCONT) ||
                  (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT) && GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT)) ||
                  // drop if buffer contains header only and has invalid timestamp
                  !GST_BUFFER_PTS_IS_VALID(buffer);

    if (!isDroppable) {
        delta = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

        frame.flags = delta ? FRAME_FLAG_NONE : FRAME_FLAG_KEY_FRAME;

        // convert from segment timestamp to running time in live mode.
        segment = gst_sample_get_segment(sample);
        buf_pts = gst_segment_to_running_time(segment, GST_FORMAT_TIME, buffer->pts);
        if (!GST_CLOCK_TIME_IS_VALID(buf_pts)) {
            printf("[KVS GStreamer Master] Frame contains invalid PTS dropping the frame. \n");
        }

        if(!(gst_buffer_map(buffer, &info, GST_MAP_READ))) {
           printf("[KVS GStreamer Master] on_new_sample(): Gst buffer mapping failed\n");
           goto CleanUp;
        }

        frame.trackId = trackid;
        frame.duration = 0;
        frame.version = FRAME_CURRENT_VERSION;
        frame.size = (UINT32) info.size;
        frame.frameData = (PBYTE) info.data;

        if (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->updatingSampleStreamingSessionList)) {
            ATOMIC_INCREMENT(&pSampleConfiguration->streamingSessionListReadingThreadCount);
            for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {

                pSampleStreamingSession = pSampleConfiguration->sampleStreamingSessionList[i];
                frame.index = (UINT32) ATOMIC_INCREMENT(&pSampleStreamingSession->frameIndex);

                if (trackid == DEFAULT_AUDIO_TRACK_ID) {
                    pRtcRtpTransceiver = pSampleStreamingSession->pAudioRtcRtpTransceiver;
                    frame.presentationTs = pSampleStreamingSession->audioTimestamp;
                    frame.decodingTs = frame.presentationTs;
                    pSampleStreamingSession->audioTimestamp += SAMPLE_AUDIO_FRAME_DURATION; // assume audio frame size is 20ms, which is default in opusenc

                } else {
                    pRtcRtpTransceiver = pSampleStreamingSession->pVideoRtcRtpTransceiver;
                    frame.presentationTs = pSampleStreamingSession->videoTimestamp;
                    frame.decodingTs = frame.presentationTs;
                    pSampleStreamingSession->videoTimestamp += SAMPLE_VIDEO_FRAME_DURATION; // assume video fps is 30
                }

                status = writeFrame(pRtcRtpTransceiver, &frame);
                if (status != STATUS_SUCCESS) {
                    #ifdef VERBOSE
                        printf("writeFrame() failed with 0x%08x", status);
                    #endif
                }
            }
            ATOMIC_DECREMENT(&pSampleConfiguration->streamingSessionListReadingThreadCount);
        }
    }

CleanUp:

    if (info.data != NULL) {
        gst_buffer_unmap(buffer, &info);
    }

    if (sample != NULL) {
        gst_sample_unref(sample);
    }

    if (ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        ret = GST_FLOW_EOS;
    }

    return ret;
}

GstFlowReturn on_new_sample_video(GstElement *sink, gpointer data) {
    return on_new_sample(sink, data, DEFAULT_VIDEO_TRACK_ID);
}

GstFlowReturn on_new_sample_audio(GstElement *sink, gpointer data) {
    return on_new_sample(sink, data, DEFAULT_AUDIO_TRACK_ID);
}

PVOID sendGstreamerAudioVideo(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    GstElement *appsinkVideo = NULL, *appsinkAudio = NULL;
    GstBus *bus;
    GstMessage *msg;
    GError *error = NULL;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) args;

    if(pSampleConfiguration == NULL) {
        printf("[KVS GStreamer Master] sendGstreamerAudioVideo(): operation returned status code: 0x%08x \n", STATUS_NULL_ARG);
        goto CleanUp;
    }

    /**
     * Use x264enc as its available on mac, pi, ubuntu and windows
     * mac pipeline fails if resolution is not 720p
     *
     * For alaw
     * audiotestsrc is-live=TRUE ! queue leaky=2 max-size-buffers=400 ! audioconvert ! audioresample !
     * audio/x-raw, rate=8000, channels=1, format=S16LE, layout=interleaved ! alawenc ! appsink sync=TRUE emit-signals=TRUE name=appsink-audio
     *
     * For VP8
     * videotestsrc is-live=TRUE ! video/x-raw,width=1280,height=720,framerate=30/1 !
     * vp8enc error-resilient=partitions keyframe-max-dist=10 auto-alt-ref=true cpu-used=5 deadline=1 !
     * appsink sync=TRUE emit-signals=TRUE name=appsink-video
     */

    switch (pSampleConfiguration->mediaType) {
        case SAMPLE_STREAMING_VIDEO_ONLY:
            if(pSampleConfiguration->useTestSrc) {
                pipeline = gst_parse_launch(
                    "videotestsrc is-live=TRUE ! queue ! videoconvert ! video/x-raw,width=1280,height=720,framerate=30/1 ! x264enc bframes=0 speed-preset=veryfast key-int-max=30 bitrate=512 ! "
                    "video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline ! appsink sync=TRUE emit-signals=TRUE name=appsink-video",
                     &error);
            }
            else {
			/*Use rpicamsrc element as source. Save stream and audio into a file*/
			pipeline = gst_parse_launch("rpicamsrc rotation=270 bitrate=1300000 ! h264parse config-interval=-1 ! video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline,width=640,height=480,framerate=30/1 ! tee name=t ! queue ! appsink sync=TRUE emit-signals=TRUE name=appsink-video "
			 "t. ! queue ! h264parse config-interval=-1 ! mux. "
			 "alsasrc device=hw:1,0 do-timestamp=true ! audio/x-raw,format=S16LE,rate=16000,channels=2 ! queue ! voaacenc ! mux. "
			 "mp4mux name=mux ! filesink location=/home/pi/Videos/cb_vid.mp4", &error);
             //"mp4mux name=mux ! filesink location=test.mp4"
             //"matroskamux name=mux ! filesink location=test.mkv"
            }
            break;

        case SAMPLE_STREAMING_AUDIO_VIDEO:
             if(pSampleConfiguration->useTestSrc) {
                 pipeline = gst_parse_launch(
                    "videotestsrc is-live=TRUE ! queue ! videoconvert ! video/x-raw,width=1280,height=720,framerate=30/1 ! x264enc bframes=0 speed-preset=veryfast key-int-max=30 bitrate=512 ! "
                    "video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline ! appsink sync=TRUE emit-signals=TRUE name=appsink-video audiotestsrc is-live=TRUE ! "
                    "queue leaky=2 max-size-buffers=400 ! audioconvert ! audioresample ! opusenc ! audio/x-opus,rate=48000,channels=2 ! appsink sync=TRUE emit-signals=TRUE name=appsink-audio",
                    &error);
            }
            else {
                pipeline = gst_parse_launch(
                    "autovideosrc ! queue ! videoconvert ! video/x-raw,width=1280,height=720,framerate=30/1 ! x264enc bframes=0 speed-preset=veryfast key-int-max=30 bitrate=512 ! "
                    "video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline ! appsink sync=TRUE emit-signals=TRUE name=appsink-video autoaudiosrc ! "
                    "queue leaky=2 max-size-buffers=400 ! audioconvert ! audioresample ! opusenc ! audio/x-opus,rate=48000,channels=2 ! appsink sync=TRUE emit-signals=TRUE name=appsink-audio",
                    &error);
            }
            break;
    }

    if(pipeline == NULL) {
        printf("[KVS GStreamer Master] sendGstreamerAudioVideo(): Failed to launch gstreamer, operation returned status code: 0x%08x \n", STATUS_INTERNAL_ERROR);
        goto CleanUp;
    }

    appsinkVideo = gst_bin_get_by_name(GST_BIN(pipeline), "appsink-video");
    appsinkAudio = gst_bin_get_by_name(GST_BIN(pipeline), "appsink-audio");

    if(!(appsinkVideo != NULL || appsinkAudio != NULL)) {
        printf("[KVS GStreamer Master] sendGstreamerAudioVideo(): cant find appsink, operation returned status code: 0x%08x \n", STATUS_INTERNAL_ERROR);
        goto CleanUp;
    }

    if (appsinkVideo != NULL) {
        g_signal_connect(appsinkVideo, "new-sample", G_CALLBACK(on_new_sample_video), (gpointer) pSampleConfiguration);
    }

    if (appsinkAudio != NULL) {
        g_signal_connect(appsinkAudio, "new-sample", G_CALLBACK(on_new_sample_audio), (gpointer) pSampleConfiguration);
    }

    gst_element_set_state (pipeline, GST_STATE_PLAYING);

    /* block until error or EOS */
    bus = gst_element_get_bus(pipeline);
    msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

    /* Free resources */
    if (msg != NULL) {
        gst_message_unref(msg);
    }
    gst_object_unref (bus);
    gst_element_set_state (pipeline, GST_STATE_NULL);
    gst_object_unref (pipeline);

CleanUp:

    if (error != NULL) {
        printf("%s", error->message);
        g_clear_error (&error);
    }

    return (PVOID) (UINT_PTR) retStatus;
}

VOID onGstAudioFrameReady(UINT64 customData, PFrame pFrame)
{
    GstFlowReturn ret;
    GstBuffer *buffer;
    GstElement *appsrcAudio = (GstElement *)(UINT32)customData;

    /* Create a new empty buffer */
    buffer = gst_buffer_new_and_alloc(pFrame->size);
    gst_buffer_fill(buffer, 0, pFrame->frameData, pFrame->size);

    /* Push the buffer into the appsrc */
    g_signal_emit_by_name(appsrcAudio, "push-buffer", buffer, &ret);

    /* Free the buffer now that we are done with it */
    gst_buffer_unref(buffer);
}

VOID onSampleStreamingSessionShutdown(UINT64 customData, PSampleStreamingSession pSampleStreamingSession)
{
    (void)(pSampleStreamingSession);
    GstElement *appsrc = (GstElement *)(UINT32)customData;
    GstFlowReturn ret;

    g_signal_emit_by_name (appsrc, "end-of-stream", &ret);
}

PVOID receiveGstreamerAudioVideo(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    GstElement *pipeline = NULL, *appsrcAudio = NULL;
    GstBus *bus;
    GstMessage *msg;
    GError *error = NULL;
    PSampleStreamingSession pSampleStreamingSession = (PSampleStreamingSession) args;
    gchar *videoDescription = "", *audioDescription = "", *audioVideoDescription;

    if(pSampleStreamingSession == NULL) {
        printf("[KVS GStreamer Master] receiveGstreamerAudioVideo(): operation returned status code: 0x%08x \n", STATUS_NULL_ARG);
        goto CleanUp;
    }

    //TODO: Wire video up with gstreamer pipeline

    switch (pSampleStreamingSession->pAudioRtcRtpTransceiver->receiver.track.codec) {
        case RTC_CODEC_OPUS:
            audioDescription = "appsrc name=appsrc-audio ! opusparse ! decodebin ! autoaudiosink";
            break;

        case RTC_CODEC_MULAW:
        case RTC_CODEC_ALAW:
            audioDescription = "appsrc name=appsrc-audio ! rawaudioparse ! decodebin ! autoaudiosink";
            break;
        default:
            break;
    }

    audioVideoDescription = g_strjoin(" ", audioDescription, videoDescription, NULL);

    pipeline = gst_parse_launch(audioVideoDescription, &error);

    appsrcAudio = gst_bin_get_by_name(GST_BIN(pipeline), "appsrc-audio");
    if(appsrcAudio == NULL) {
        printf("[KVS GStreamer Master] gst_bin_get_by_name(): cant find appsrc, operation returned status code: 0x%08x \n", STATUS_INTERNAL_ERROR);
        goto CleanUp;
    }

    transceiverOnFrame(pSampleStreamingSession->pAudioRtcRtpTransceiver,
                       (UINT32) appsrcAudio,
                       onGstAudioFrameReady);

    retStatus = streamingSessionOnShutdown(pSampleStreamingSession, (UINT32) appsrcAudio, onSampleStreamingSessionShutdown);
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS GStreamer Master] streamingSessionOnShutdown(): operation returned status code: 0x%08x \n", STATUS_INTERNAL_ERROR);
        goto CleanUp;
    }

    g_free(audioVideoDescription);

    if(pipeline == NULL) {
        printf("[KVS GStreamer Master] receiveGstreamerAudioVideo(): Failed to launch gstreamer, operation returned status code: 0x%08x \n", STATUS_INTERNAL_ERROR);
        goto CleanUp;
    }

    gst_element_set_state (pipeline, GST_STATE_PLAYING);

    /* block until error or EOS */
    bus = gst_element_get_bus(pipeline);
    msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

    /* Free resources */
    if (msg != NULL) {
        gst_message_unref(msg);
    }
    gst_object_unref (bus);
    gst_element_set_state (pipeline, GST_STATE_NULL);
    gst_object_unref (pipeline);

CleanUp:
    if (error != NULL) {
        printf("%s", error->message);
        g_clear_error (&error);
    }

    return (PVOID) (UINT_PTR) retStatus;
}

INT32 main(INT32 argc, CHAR *argv[])
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = NULL;

    signal(SIGINT, sigintHandler);

    // do trickle-ice by default
    printf("[KVS GStreamer Master] Using trickleICE by default\n");

    retStatus = createSampleConfiguration(argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME,
                                          SIGNALING_CHANNEL_ROLE_TYPE_MASTER,
                                          TRUE,
                                          TRUE,
                                          &pSampleConfiguration);
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS GStreamer Master] createSampleConfiguration(): operation returned status code: 0x%08x \n", retStatus);
        goto CleanUp;
    }

    printf("[KVS GStreamer Master] Created signaling channel %s\n", (argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME));

    pSampleConfiguration->videoSource = sendGstreamerAudioVideo;
    pSampleConfiguration->mediaType = SAMPLE_STREAMING_VIDEO_ONLY;
    pSampleConfiguration->receiveAudioVideoSource = receiveGstreamerAudioVideo;
    pSampleConfiguration->fun_ptr = send_eos;/*assign function address to fun_ptr*/
    pSampleConfiguration->onDataChannel = onDataChannel;
    pSampleConfiguration->customData = (UINT32) pSampleConfiguration;
    pSampleConfiguration->useTestSrc = FALSE;
    /* Initialize GStreamer */
    gst_init(&argc, &argv);
    printf("[KVS Gstreamer Master] Finished initializing GStreamer\n");

    if (argc > 2) {
        if (STRCMP(argv[2], "video-only") == 0) {
            pSampleConfiguration->mediaType = SAMPLE_STREAMING_VIDEO_ONLY;
            printf("[KVS Gstreamer Master] Streaming video only\n");
        } else if (STRCMP(argv[2], "audio-video") == 0) {
            pSampleConfiguration->mediaType = SAMPLE_STREAMING_AUDIO_VIDEO;
            printf("[KVS Gstreamer Master] Streaming audio and video\n");
        } else {
            printf("[KVS Gstreamer Master] Unrecognized streaming type. Default to video-only\n");
        }
    }
    else {
        printf("[KVS Gstreamer Master] Streaming video only\n");
    }

//    if(argc > 3) {
//        if (STRCMP(argv[3], "testsrc") == 0) {
//            printf("[KVS GStreamer Master] Using test source in GStreamer\n");
//            pSampleConfiguration->useTestSrc = TRUE;
//        }
//    }


    switch (pSampleConfiguration->mediaType) {
        case SAMPLE_STREAMING_VIDEO_ONLY:
            printf("[KVS GStreamer Master] streaming type video-only\n");
            break;
        case SAMPLE_STREAMING_AUDIO_VIDEO:
            printf("[KVS GStreamer Master] streaming type audio-video\n");
            break;
    }

    // Initalize KVS WebRTC. This must be done before anything else, and must only be done once.
    retStatus = initKvsWebRtc();
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS GStreamer Master] initKvsWebRtc(): operation returned status code: 0x%08x \n", retStatus);
        goto CleanUp;
    }
    printf("[KVS GStreamer Master] KVS WebRTC initialization completed successfully\n");

    pSampleConfiguration->signalingClientCallbacks.messageReceivedFn = masterMessageReceived;

    strcpy(pSampleConfiguration->clientInfo.clientId, SAMPLE_MASTER_CLIENT_ID);

    retStatus = createSignalingClientSync(&pSampleConfiguration->clientInfo, &pSampleConfiguration->channelInfo,
                                          &pSampleConfiguration->signalingClientCallbacks, pSampleConfiguration->pCredentialProvider,
                                          &pSampleConfiguration->signalingClientHandle);
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS GStreamer Master] createSignalingClientSync(): operation returned status code: 0x%08x \n", retStatus);
    }
    printf("[KVS GStreamer Master] Signaling client created successfully\n");

    // Enable the processing of the messages
    retStatus = signalingClientConnectSync(pSampleConfiguration->signalingClientHandle);
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS GStreamer Master] signalingClientConnectSync(): operation returned status code: 0x%08x \n", retStatus);
        goto CleanUp;
    }
    printf("[KVS GStreamer Master] Signaling client connection to socket established\n");

    /*open the pipeline to enable saving setream to file on application start*/
    printf("[KVS Master] Streaming to KVS stream %s\n", argv[3]);
    CHK_STATUS(startSenderMediaThreads(pSampleConfiguration));

    printf("[KVS Gstreamer Master] Beginning streaming...check the stream over channel %s\n",
            (argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME));

    gSampleConfiguration = pSampleConfiguration;

    // Checking for termination
    retStatus = sessionCleanupWait(pSampleConfiguration);
    if(retStatus != STATUS_SUCCESS) {
        printf("[KVS GStreamer Master] sessionCleanupWait(): operation returned status code: 0x%08x \n", retStatus);
        goto CleanUp;
    }

    printf("[KVS GStreamer Master] Streaming session terminated\n");

CleanUp:

    if (retStatus != STATUS_SUCCESS) {
        printf("[KVS GStreamer Master] Terminated with status code 0x%08x", retStatus);
    }

    printf("[KVS GStreamer Master] Cleaning up....\n");

    if (pSampleConfiguration != NULL) {

        // Kick of the termination sequence
        ATOMIC_STORE_BOOL(&pSampleConfiguration->appTerminateFlag, TRUE);

        if (pSampleConfiguration->videoSenderTid != (UINT32) NULL) {
           // Join the threads
            THREAD_JOIN(pSampleConfiguration->videoSenderTid, NULL);
        }

        retStatus = freeSignalingClient(&pSampleConfiguration->signalingClientHandle);
        if(retStatus != STATUS_SUCCESS) {
            printf("[KVS GStreamer Master] freeSignalingClient(): operation returned status code: 0x%08x \n", retStatus);
        }

        retStatus = freeSampleConfiguration(&pSampleConfiguration);
        if(retStatus != STATUS_SUCCESS) {
            printf("[KVS GStreamer Master] freeSampleConfiguration(): operation returned status code: 0x%08x \n", retStatus);
        }
    }
    printf("[KVS Gstreamer Master] Cleanup done\n");
	char str_temp[32];
	sprintf(str_temp, "KVS_STREAMER_REV=%s\n", KVS_STREAMER_REV);
    printf(str_temp);
    return (INT32) retStatus;
}

void send_eos(void)
{
	printf("\n[KVS GStreamer Master] Send EOS to pipeline \n");
	gst_element_send_event (pipeline, gst_event_new_eos());
}
