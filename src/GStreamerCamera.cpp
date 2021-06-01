//
// Created by liuzikai on 3/11/21.
//
#include "GStreamerCamera.h"
#include <iostream>
namespace meta {

GStreamerCamera::GStreamerCamera() {
    gst_init(nullptr, nullptr);
}

bool GStreamerCamera::open(const package::ParamSet &params) {
    // Start the fetching thread
    if (th) {
        close();
    }
    threadShouldExit = false;
    th = new std::thread(&GStreamerCamera::readFrameFromCamera, this, params);

    return true;
}

GStreamerCamera::~GStreamerCamera() {
    if (th) {
        threadShouldExit = true;
        th->join();
        delete th;
        th = nullptr;
    }
}

void GStreamerCamera::readFrameFromCamera(const package::ParamSet &params) {
    capInfoSS.str(std::string());

    // Open the camera in the same thread
    cap.open(params.camera_id(), cv::CAP_ANY);
    // cv::VideoCapture cap("v4l2src device=/dev/video0 io-mode=2 ! image/jpeg, width=(int)1280, height=(int)720 ! nvjpegdec ! video/x-raw ! videoconvert ! video/x-raw,format=BGR ! appsink", cv::CAP_GSTREAMER);
    if (!cap.isOpened()) {
        capInfoSS << "Failed to open camera " << params.camera_id() << "\n";
        std::cerr << capInfoSS.rdbuf();
        return;
    }

    // Set parameters
    if (!cap.set(cv::CAP_PROP_FRAME_WIDTH, params.image_width())) {
        capInfoSS << "Failed to set width.\n";
    }
    if (!cap.set(cv::CAP_PROP_FRAME_HEIGHT, params.image_height())) {
        capInfoSS << "Failed to set height.\n";
    }
    if (!cap.set(cv::CAP_PROP_FPS, params.fps())) {
        capInfoSS << "Failed to set fps.\n";
    }
    if (params.gamma().enabled()) {
        if (!cap.set(cv::CAP_PROP_GAMMA, params.gamma().val())) {
            capInfoSS << "Failed to set gamma.\n";
        }
    }

    // Get a test frame
    cap.read(buffer[0]);
    if (buffer->empty()) {
        capInfoSS << "Failed to fetch test image from camera " << params.camera_id() << "\n";
        std::cerr << capInfoSS.rdbuf();
        return;
    }
    if (buffer[0].cols != params.image_width() || buffer[0].rows != params.image_height()) {
        capInfoSS << "Invalid frame size. "
                  << "Expected: " << params.image_width() << "x" << params.image_height() << ", "
                  << "Actual: " << buffer[0].cols << "x" << buffer[0].rows << "\n";
        std::cerr << capInfoSS.rdbuf();
        return;
    }

    // Report actual parameters
    capInfoSS << "Camera " << params.camera_id() << ", "
              << cap.get(cv::CAP_PROP_FRAME_WIDTH) << "x" << cap.get(cv::CAP_PROP_FRAME_HEIGHT)
              << " @ " << cap.get(cv::CAP_PROP_FPS) << " fps\n"
              << "Gamma: " << cap.get(cv::CAP_PROP_GAMMA) << "\n";
    std::cout << capInfoSS.rdbuf();

    while (true) {

        uint8_t workingBuffer = 1 - lastBuffer;

        if (threadShouldExit || !cap.isOpened()) {
            bufferFrameID[workingBuffer] = -1;  // indicate invalid frame
            break;
        }

        if (!cap.read(buffer[workingBuffer])) {
            continue;  // try again
        }

        bufferFrameID[workingBuffer] = bufferFrameID[lastBuffer] + 1;
        if (bufferFrameID[workingBuffer] >= FRAME_ID_MAX) bufferFrameID[workingBuffer] = 0;
        lastBuffer = workingBuffer;

        ++cumulativeFrameCounter;  // the only place of incrementing
    }

    cap.release();
    std::cout << "Camera: closed\n";
}

void GStreamerCamera::handleAppSinkEOS(GstAppSink *appSink, gpointer userData) {
    threadShouldExit = true;
}

void GStreamerCamera::close() {
    if (th) {
        threadShouldExit = true;
        th->join();
        delete th;
        th = nullptr;
    }
}

}