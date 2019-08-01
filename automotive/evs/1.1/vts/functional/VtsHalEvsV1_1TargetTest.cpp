/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "VtsHalEvsTest"


// Note:  We have't got a great way to indicate which target
// should be tested, so we'll leave the interface served by the
// default (mock) EVS driver here for easy reference.  All
// actual EVS drivers should serve on the EvsEnumeratorHw name,
// however, so the code is checked in that way.
//const static char kEnumeratorName[]  = "EvsEnumeratorHw-Mock";
const static char kEnumeratorName[]  = "EvsEnumeratorHw";


// These values are called out in the EVS design doc (as of Mar 8, 2017)
static const int kMaxStreamStartMilliseconds = 500;
static const int kMinimumFramesPerSecond = 10;

static const int kSecondsToMilliseconds = 1000;
static const int kMillisecondsToMicroseconds = 1000;
static const float kNanoToMilliseconds = 0.000001f;
static const float kNanoToSeconds = 0.000000001f;


#include "FrameHandler.h"

#include <stdio.h>
#include <string.h>

#include <hidl/HidlTransportSupport.h>
#include <hwbinder/ProcessState.h>
#include <log/log.h>
#include <utils/Errors.h>
#include <utils/StrongPointer.h>

#include <android/log.h>
#include <android/hardware/automotive/evs/1.1/IEvsCamera.h>
#include <android/hardware/automotive/evs/1.1/IEvsCameraStream.h>
#include <android/hardware/automotive/evs/1.0/IEvsEnumerator.h>
#include <android/hardware/automotive/evs/1.0/IEvsDisplay.h>

#include <VtsHalHidlTargetTestBase.h>
#include <VtsHalHidlTargetTestEnvBase.h>

using namespace ::android::hardware::automotive::evs::V1_1;

using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::hardware::hidl_vec;
using ::android::hardware::hidl_handle;
using ::android::hardware::hidl_string;
using ::android::sp;
using ::android::hardware::automotive::evs::V1_0::CameraDesc;
using ::android::hardware::automotive::evs::V1_0::DisplayDesc;
using ::android::hardware::automotive::evs::V1_0::DisplayState;
using ::android::hardware::automotive::evs::V1_0::IEvsEnumerator;
using IEvsCamera_1_0 = ::android::hardware::automotive::evs::V1_0::IEvsCamera;
using IEvsCamera_1_1 = ::android::hardware::automotive::evs::V1_1::IEvsCamera;

// Test environment for Evs HIDL HAL.
class EvsHidlEnvironment : public ::testing::VtsHalHidlTargetTestEnvBase {
   public:
    // get the test environment singleton
    static EvsHidlEnvironment* Instance() {
        static EvsHidlEnvironment* instance = new EvsHidlEnvironment;
        return instance;
    }

    virtual void registerTestServices() override { registerTestService<IEvsEnumerator>(); }

   private:
    EvsHidlEnvironment() {}
};

// The main test class for EVS
class EvsHidlTest : public ::testing::VtsHalHidlTargetTestBase {
public:
    virtual void SetUp() override {
        // Make sure we can connect to the enumerator
        string service_name =
            EvsHidlEnvironment::Instance()->getServiceName<IEvsEnumerator>(kEnumeratorName);
        pEnumerator = getService<IEvsEnumerator>(service_name);
        ASSERT_NE(pEnumerator.get(), nullptr);

        mIsHwModule = !service_name.compare(kEnumeratorName);
    }

    virtual void TearDown() override {}

protected:
    void loadCameraList() {
        // SetUp() must run first!
        assert(pEnumerator != nullptr);

        // Get the camera list
        pEnumerator->getCameraList([this](hidl_vec <CameraDesc> cameraList) {
                                       ALOGI("Camera list callback received %zu cameras",
                                             cameraList.size());
                                       cameraInfo.reserve(cameraList.size());
                                       for (auto&& cam: cameraList) {
                                           ALOGI("Found camera %s", cam.cameraId.c_str());
                                           cameraInfo.push_back(cam);
                                       }
                                   }
        );

        // We insist on at least one camera for EVS to pass any camera tests
        ASSERT_GE(cameraInfo.size(), 1u);
    }

    sp<IEvsEnumerator>        pEnumerator;    // Every test needs access to the service
    std::vector <CameraDesc>  cameraInfo;     // Empty unless/until loadCameraList() is called
    bool                        mIsHwModule;    // boolean to tell current module under testing
                                                // is HW module implementation.
};


// Test cases, their implementations, and corresponding requirements are
// documented at go/aae-evs-public-api-test.

/*
 * CameraOpenClean:
 * Opens each camera reported by the enumerator and then explicitly closes it via a
 * call to closeCamera.  Then repeats the test to ensure all cameras can be reopened.
 */
TEST_F(EvsHidlTest, CameraOpenClean) {
    ALOGI("Starting CameraOpenClean test");

    // Get the camera list
    loadCameraList();

    // Open and close each camera twice
    for (auto&& cam: cameraInfo) {
        for (int pass = 0; pass < 2; pass++) {
            sp<IEvsCamera_1_1> pCam =
                IEvsCamera_1_1::castFrom(pEnumerator->openCamera(cam.cameraId))
                .withDefault(nullptr);
            ASSERT_NE(pCam, nullptr);

            // Verify that this camera self-identifies correctly
            pCam->getCameraInfo([&cam](CameraDesc desc) {
                                    ALOGD("Found camera %s", desc.cameraId.c_str());
                                    EXPECT_EQ(cam.cameraId, desc.cameraId);
                                }
            );

            // Explicitly close the camera so resources are released right away
            pEnumerator->closeCamera(pCam);
        }
    }
}


/*
 * CameraOpenAggressive:
 * Opens each camera reported by the enumerator twice in a row without an intervening closeCamera
 * call.  This ensures that the intended "aggressive open" behavior works.  This is necessary for
 * the system to be tolerant of shutdown/restart race conditions.
 */
TEST_F(EvsHidlTest, CameraOpenAggressive) {
    ALOGI("Starting CameraOpenAggressive test");

    // Get the camera list
    loadCameraList();

    // Open and close each camera twice
    for (auto&& cam: cameraInfo) {
        sp<IEvsCamera_1_1> pCam =
            IEvsCamera_1_1::castFrom(pEnumerator->openCamera(cam.cameraId))
            .withDefault(nullptr);
        ASSERT_NE(pCam, nullptr);

        // Verify that this camera self-identifies correctly
        pCam->getCameraInfo([&cam](CameraDesc desc) {
                                ALOGD("Found camera %s", desc.cameraId.c_str());
                                EXPECT_EQ(cam.cameraId, desc.cameraId);
                            }
        );

        sp<IEvsCamera_1_1> pCam2 =
            IEvsCamera_1_1::castFrom(pEnumerator->openCamera(cam.cameraId))
            .withDefault(nullptr);
        ASSERT_NE(pCam, pCam2);
        ASSERT_NE(pCam2, nullptr);

        Return<EvsResult> result = pCam->setMaxFramesInFlight(2);
        if (mIsHwModule) {
            // Verify that the old camera rejects calls via HW module.
            EXPECT_EQ(EvsResult::OWNERSHIP_LOST, EvsResult(result));
        } else {
            // default implementation supports multiple clients.
            EXPECT_EQ(EvsResult::OK, EvsResult(result));
        }

        // Close the superceded camera
        pEnumerator->closeCamera(pCam);

        // Verify that the second camera instance self-identifies correctly
        pCam2->getCameraInfo([&cam](CameraDesc desc) {
                                 ALOGD("Found camera %s", desc.cameraId.c_str());
                                 EXPECT_EQ(cam.cameraId, desc.cameraId);
                             }
        );

        // Close the second camera instance
        pEnumerator->closeCamera(pCam2);
    }

    // Sleep here to ensure the destructor cleanup has time to run so we don't break follow on tests
    sleep(1);   // I hate that this is an arbitrary time to wait.  :(  b/36122635
}


/*
 * CameraStreamPerformance:
 * Measure and qualify the stream start up time and streaming frame rate of each reported camera
 */
TEST_F(EvsHidlTest, CameraStreamPerformance) {
    ALOGI("Starting CameraStreamPerformance test");

    // Get the camera list
    loadCameraList();

    // Test each reported camera
    for (auto&& cam: cameraInfo) {
        sp<IEvsCamera_1_1> pCam =
            IEvsCamera_1_1::castFrom(pEnumerator->openCamera(cam.cameraId))
            .withDefault(nullptr);
        ASSERT_NE(pCam, nullptr);

        // Set up a frame receiver object which will fire up its own thread
        sp<FrameHandler> frameHandler = new FrameHandler(pCam, cam,
                                                         nullptr,
                                                         FrameHandler::eAutoReturn);

        // Start the camera's video stream
        nsecs_t start = systemTime(SYSTEM_TIME_MONOTONIC);
        bool startResult = frameHandler->startStream();
        ASSERT_TRUE(startResult);

        // Ensure the first frame arrived within the expected time
        frameHandler->waitForFrameCount(1);
        nsecs_t firstFrame = systemTime(SYSTEM_TIME_MONOTONIC);
        nsecs_t timeToFirstFrame = systemTime(SYSTEM_TIME_MONOTONIC) - start;
        EXPECT_LE(nanoseconds_to_milliseconds(timeToFirstFrame), kMaxStreamStartMilliseconds);
        printf("Measured time to first frame %0.2f ms\n", timeToFirstFrame * kNanoToMilliseconds);
        ALOGI("Measured time to first frame %0.2f ms", timeToFirstFrame * kNanoToMilliseconds);

        // Check aspect ratio
        unsigned width = 0, height = 0;
        frameHandler->getFrameDimension(&width, &height);
        EXPECT_GE(width, height);

        // Wait a bit, then ensure we get at least the required minimum number of frames
        sleep(5);
        nsecs_t end = systemTime(SYSTEM_TIME_MONOTONIC);
        unsigned framesReceived = 0;
        frameHandler->getFramesCounters(&framesReceived, nullptr);
        framesReceived = framesReceived - 1;    // Back out the first frame we already waited for
        nsecs_t runTime = end - firstFrame;
        float framesPerSecond = framesReceived / (runTime * kNanoToSeconds);
        printf("Measured camera rate %3.2f fps\n", framesPerSecond);
        ALOGI("Measured camera rate %3.2f fps", framesPerSecond);
        EXPECT_GE(framesPerSecond, kMinimumFramesPerSecond);

        // Even when the camera pointer goes out of scope, the FrameHandler object will
        // keep the stream alive unless we tell it to shutdown.
        // Also note that the FrameHandle and the Camera have a mutual circular reference, so
        // we have to break that cycle in order for either of them to get cleaned up.
        frameHandler->shutdown();

        // Explicitly release the camera
        pEnumerator->closeCamera(pCam);
    }
}


/*
 * CameraStreamBuffering:
 * Ensure the camera implementation behaves properly when the client holds onto buffers for more
 * than one frame time.  The camera must cleanly skip frames until the client is ready again.
 */
TEST_F(EvsHidlTest, CameraStreamBuffering) {
    ALOGI("Starting CameraStreamBuffering test");

    // Arbitrary constant (should be > 1 and less than crazy)
    static const unsigned int kBuffersToHold = 6;

    // Get the camera list
    loadCameraList();

    // Test each reported camera
    for (auto&& cam: cameraInfo) {

        sp<IEvsCamera_1_1> pCam =
            IEvsCamera_1_1::castFrom(pEnumerator->openCamera(cam.cameraId))
            .withDefault(nullptr);
        ASSERT_NE(pCam, nullptr);

        // Ask for a crazy number of buffers in flight to ensure it errors correctly
        Return<EvsResult> badResult = pCam->setMaxFramesInFlight(0xFFFFFFFF);
        EXPECT_EQ(EvsResult::BUFFER_NOT_AVAILABLE, badResult);

        // Now ask for exactly two buffers in flight as we'll test behavior in that case
        Return<EvsResult> goodResult = pCam->setMaxFramesInFlight(kBuffersToHold);
        EXPECT_EQ(EvsResult::OK, goodResult);


        // Set up a frame receiver object which will fire up its own thread.
        sp<FrameHandler> frameHandler = new FrameHandler(pCam, cam,
                                                         nullptr,
                                                         FrameHandler::eNoAutoReturn);

        // Start the camera's video stream
        bool startResult = frameHandler->startStream();
        ASSERT_TRUE(startResult);

        // Check that the video stream stalls once we've gotten exactly the number of buffers
        // we requested since we told the frameHandler not to return them.
        sleep(2);   // 1 second should be enough for at least 5 frames to be delivered worst case
        unsigned framesReceived = 0;
        frameHandler->getFramesCounters(&framesReceived, nullptr);
        ASSERT_EQ(kBuffersToHold, framesReceived) << "Stream didn't stall at expected buffer limit";


        // Give back one buffer
        bool didReturnBuffer = frameHandler->returnHeldBuffer();
        EXPECT_TRUE(didReturnBuffer);

        // Once we return a buffer, it shouldn't take more than 1/10 second to get a new one
        // filled since we require 10fps minimum -- but give a 10% allowance just in case.
        usleep(110 * kMillisecondsToMicroseconds);
        frameHandler->getFramesCounters(&framesReceived, nullptr);
        EXPECT_EQ(kBuffersToHold+1, framesReceived) << "Stream should've resumed";

        // Even when the camera pointer goes out of scope, the FrameHandler object will
        // keep the stream alive unless we tell it to shutdown.
        // Also note that the FrameHandle and the Camera have a mutual circular reference, so
        // we have to break that cycle in order for either of them to get cleaned up.
        frameHandler->shutdown();

        // Explicitly release the camera
        pEnumerator->closeCamera(pCam);
    }
}


/*
 * CameraToDisplayRoundTrip:
 * End to end test of data flowing from the camera to the display.  Each delivered frame of camera
 * imagery is simply copied to the display buffer and presented on screen.  This is the one test
 * which a human could observe to see the operation of the system on the physical display.
 */
TEST_F(EvsHidlTest, CameraToDisplayRoundTrip) {
    ALOGI("Starting CameraToDisplayRoundTrip test");

    // Get the camera list
    loadCameraList();

    // Request exclusive access to the EVS display
    sp<IEvsDisplay> pDisplay = pEnumerator->openDisplay();
    ASSERT_NE(pDisplay, nullptr);

    // Test each reported camera
    for (auto&& cam: cameraInfo) {
        sp<IEvsCamera_1_1> pCam =
            IEvsCamera_1_1::castFrom(pEnumerator->openCamera(cam.cameraId))
            .withDefault(nullptr);
        ASSERT_NE(pCam, nullptr);

        // Set up a frame receiver object which will fire up its own thread.
        sp<FrameHandler> frameHandler = new FrameHandler(pCam, cam,
                                                         pDisplay,
                                                         FrameHandler::eAutoReturn);


        // Activate the display
        pDisplay->setDisplayState(DisplayState::VISIBLE_ON_NEXT_FRAME);

        // Start the camera's video stream
        bool startResult = frameHandler->startStream();
        ASSERT_TRUE(startResult);

        // Wait a while to let the data flow
        static const int kSecondsToWait = 5;
        const int streamTimeMs = kSecondsToWait * kSecondsToMilliseconds -
                                 kMaxStreamStartMilliseconds;
        const unsigned minimumFramesExpected = streamTimeMs * kMinimumFramesPerSecond /
                                               kSecondsToMilliseconds;
        sleep(kSecondsToWait);
        unsigned framesReceived = 0;
        unsigned framesDisplayed = 0;
        frameHandler->getFramesCounters(&framesReceived, &framesDisplayed);
        EXPECT_EQ(framesReceived, framesDisplayed);
        EXPECT_GE(framesDisplayed, minimumFramesExpected);

        // Turn off the display (yes, before the stream stops -- it should be handled)
        pDisplay->setDisplayState(DisplayState::NOT_VISIBLE);

        // Shut down the streamer
        frameHandler->shutdown();

        // Explicitly release the camera
        pEnumerator->closeCamera(pCam);
    }

    // Explicitly release the display
    pEnumerator->closeDisplay(pDisplay);
}


/*
 * MultiCameraStream:
 * Verify that each client can start and stop video streams on the same
 * underlying camera.
 */
TEST_F(EvsHidlTest, MultiCameraStream) {
    ALOGI("Starting MultiCameraStream test");

    if (mIsHwModule) {
        // This test is not for HW module implementation.
        return;
    }

    // Get the camera list
    loadCameraList();

    // Test each reported camera
    for (auto&& cam: cameraInfo) {
        // Create two camera clients.
        sp<IEvsCamera_1_1> pCam0 =
            IEvsCamera_1_1::castFrom(pEnumerator->openCamera(cam.cameraId))
            .withDefault(nullptr);
        ASSERT_NE(pCam0, nullptr);

        sp<IEvsCamera_1_1> pCam1 =
            IEvsCamera_1_1::castFrom(pEnumerator->openCamera(cam.cameraId))
            .withDefault(nullptr);
        ASSERT_NE(pCam1, nullptr);

        // Set up per-client frame receiver objects which will fire up its own thread
        sp<FrameHandler> frameHandler0 = new FrameHandler(pCam0, cam,
                                                          nullptr,
                                                          FrameHandler::eAutoReturn);
        ASSERT_NE(frameHandler0, nullptr);

        sp<FrameHandler> frameHandler1 = new FrameHandler(pCam1, cam,
                                                          nullptr,
                                                          FrameHandler::eAutoReturn);
        ASSERT_NE(frameHandler1, nullptr);

        // Start the camera's video stream via client 0
        bool startResult = false;
        startResult = frameHandler0->startStream() &&
                      frameHandler1->startStream();
        ASSERT_TRUE(startResult);

        // Ensure the stream starts
        frameHandler0->waitForFrameCount(1);
        frameHandler1->waitForFrameCount(1);

        nsecs_t firstFrame = systemTime(SYSTEM_TIME_MONOTONIC);

        // Wait a bit, then ensure both clients get at least the required minimum number of frames
        sleep(5);
        nsecs_t end = systemTime(SYSTEM_TIME_MONOTONIC);
        unsigned framesReceived0 = 0, framesReceived1 = 0;
        frameHandler0->getFramesCounters(&framesReceived0, nullptr);
        frameHandler1->getFramesCounters(&framesReceived1, nullptr);
        framesReceived0 = framesReceived0 - 1;    // Back out the first frame we already waited for
        framesReceived1 = framesReceived1 - 1;    // Back out the first frame we already waited for
        nsecs_t runTime = end - firstFrame;
        float framesPerSecond0 = framesReceived0 / (runTime * kNanoToSeconds);
        float framesPerSecond1 = framesReceived1 / (runTime * kNanoToSeconds);
        printf("Measured camera rate %3.2f fps and %3.2f fps\n", framesPerSecond0, framesPerSecond1);
        ALOGI("Measured camera rate %3.2f fps and %3.2f fps", framesPerSecond0, framesPerSecond1);
        EXPECT_GE(framesPerSecond0, kMinimumFramesPerSecond);
        EXPECT_GE(framesPerSecond1, kMinimumFramesPerSecond);

        // Shutdown one client
        frameHandler0->shutdown();

        // Read frame counters again
        frameHandler0->getFramesCounters(&framesReceived0, nullptr);
        frameHandler1->getFramesCounters(&framesReceived1, nullptr);

        // Wait a bit again
        sleep(5);
        unsigned framesReceivedAfterStop0 = 0, framesReceivedAfterStop1 = 0;
        frameHandler0->getFramesCounters(&framesReceivedAfterStop0, nullptr);
        frameHandler1->getFramesCounters(&framesReceivedAfterStop1, nullptr);
        EXPECT_EQ(framesReceived0, framesReceivedAfterStop0);
        EXPECT_LT(framesReceived1, framesReceivedAfterStop1);

        // Shutdown another
        frameHandler1->shutdown();

        // Explicitly release the camera
        pEnumerator->closeCamera(pCam0);
        pEnumerator->closeCamera(pCam1);
    }
}


int main(int argc, char** argv) {
    ::testing::AddGlobalTestEnvironment(EvsHidlEnvironment::Instance());
    ::testing::InitGoogleTest(&argc, argv);
    EvsHidlEnvironment::Instance()->init(&argc, argv);
    int status = RUN_ALL_TESTS();
    ALOGI("Test result = %d", status);
    return status;
}
