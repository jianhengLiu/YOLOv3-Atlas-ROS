/**
* Copyright 2020 Huawei Technologies Co., Ltd
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at

* http://www.apache.org/licenses/LICENSE-2.0

* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.

* File sample_process.cpp
* Description: handle acl resource
*/
#include "object_detect.h"
#include <iostream>
#include "model_process.h"
#include "acl/acl.h"
#include "utils.h"

//共享内存
#include "smobject.h"

//共享内存写对象,端口号66，传输对象改为BoundingBox
static shmobject shmobj2(10+sizeof(BoundingBox),10,66);
unsigned char* shmdata=NULL;

using namespace std;

namespace {
const static std::vector<std::string> yolov3Label = { "person", "bicycle", "car", "motorbike",
"aeroplane","bus", "train", "truck", "boat",
"traffic light", "fire hydrant", "stop sign", "parking meter",
"bench", "bird", "cat", "dog", "horse",
"sheep", "cow", "elephant", "bear", "zebra",
"giraffe", "backpack", "umbrella", "handbag","tie",
"suitcase", "frisbee", "skis", "snowboard", "sports ball",
"kite", "baseball bat", "baseball glove", "skateboard", "surfboard",
"tennis racket", "bottle", "wine glass", "cup",
"fork", "knife", "spoon", "bowl", "banana",
"apple", "sandwich", "orange", "broccoli", "carrot",
"hot dog", "pizza", "donut", "cake", "chair",
"sofa", "potted plant", "bed", "dining table", "toilet",
"TV monitor", "laptop", "mouse", "remote", "keyboard",
"cell phone", "microwave", "oven", "toaster", "sink",
"refrigerator", "book", "clock", "vase","scissors",
"teddy bear", "hair drier", "toothbrush" };
//Inferential output dataset subscript 0 unit is detection box information data
const uint32_t kBBoxDataBufId = 0;
//The unit with subscript 1 is the number of boxes
const uint32_t kBoxNumDataBufId = 1;
//Each field subscript in the box message
enum BBoxIndex { TOPLEFTX = 0, TOPLEFTY, BOTTOMRIGHTX, BOTTOMRIGHTY, SCORE, LABEL };
}

//nms后处理方法参数
const uint numClasses = 80; 
const float nmsThresh = 0.45;

ObjectDetect::ObjectDetect(const char* modelPath, uint32_t modelWidth,
                           uint32_t modelHeight)
:deviceId_(0), imageDataBuf_(nullptr), imageInfoBuf_(nullptr), modelWidth_(modelWidth),
modelHeight_(modelHeight), channel_(nullptr), isInited_(false){
    modelPath_ = modelPath;
    imageDataSize_ = RGBU8_IMAGE_SIZE(modelWidth, modelHeight);
}

ObjectDetect::~ObjectDetect() {
    DestroyResource();
    //系统关闭，释放内存
    shmobj2.writerelease();
}

Result ObjectDetect::InitResource() {
    // ACL init
    const char *aclConfigPath = "../src/acl.json";
    aclError ret = aclInit(aclConfigPath);
    if (ret != ACL_ERROR_NONE) {
        ERROR_LOG("Acl init failed");
        return FAILED;
    }
    INFO_LOG("Acl init success");

    // open device
    ret = aclrtSetDevice(deviceId_);
    if (ret != ACL_ERROR_NONE) {
        ERROR_LOG("Acl open device %d failed", deviceId_);
        return FAILED;
    }
    INFO_LOG("Open device %d success", deviceId_);
    //Gets whether the current application is running on host or Device
    ret = aclrtGetRunMode(&runMode_);
    if (ret != ACL_ERROR_NONE) {
        ERROR_LOG("acl get run mode failed");
        return FAILED;
    }

    return SUCCESS;
}

Result ObjectDetect::InitModel(const char* omModelPath) {
    Result ret = model_.LoadModelFromFileWithMem(omModelPath);
    if (ret != SUCCESS) {
        ERROR_LOG("execute LoadModelFromFileWithMem failed");
        return FAILED;
    }

    ret = model_.CreateDesc();
    if (ret != SUCCESS) {
        ERROR_LOG("execute CreateDesc failed");
        return FAILED;
    }

    ret = model_.CreateOutput();
    if (ret != SUCCESS) {
        ERROR_LOG("execute CreateOutput failed");
        return FAILED;
    }


    return SUCCESS;
}

Result ObjectDetect::CreateModelInputdDataset()
{
    //Request image data memory for input model
    aclError aclRet = aclrtMalloc(&imageDataBuf_, imageDataSize_, ACL_MEM_MALLOC_HUGE_FIRST);
    if (aclRet != ACL_ERROR_NONE) {
        ERROR_LOG("malloc device data buffer failed, aclRet is %d", aclRet);
        return FAILED;
    }
    //The second input to Yolov3 is the input image width and height parameter
    const float imageInfo[4] = {(float)modelWidth_, (float)modelHeight_,
    (float)modelWidth_, (float)modelHeight_};
    imageInfoSize_ = sizeof(imageInfo);
    if (runMode_ == ACL_HOST)
        imageInfoBuf_ = Utils::CopyDataHostToDevice((void *)imageInfo, imageInfoSize_);
    else
        imageInfoBuf_ = Utils::CopyDataDeviceToDevice((void *)imageInfo, imageInfoSize_);
    if (imageInfoBuf_ == nullptr) {
        ERROR_LOG("Copy image info to device failed");
        return FAILED;
    }
    //Use the applied memory to create the model and input dataset. After creation, only update the memory data for each frame of inference, instead of creating the input dataset every time
    Result ret = model_.CreateInput(imageDataBuf_, imageDataSize_,
    imageInfoBuf_, imageInfoSize_);
    if (ret != SUCCESS) {
        ERROR_LOG("Create mode input dataset failed");
        return FAILED;
    }

    return SUCCESS;
}

Result ObjectDetect::OpenPresenterChannel() {
    OpenChannelParam param;
    param.host_ip = "192.168.1.223";  //IP address of Presenter Server
    param.port = 7006;  //port of present service
    param.channel_name = "video";
    param.content_type = ContentType::kVideo;  //content type is Video
    INFO_LOG("OpenChannel start");
    PresenterErrorCode errorCode = OpenChannel(channel_, param);
    INFO_LOG("OpenChannel param");
    if (errorCode != PresenterErrorCode::kNone) {
        ERROR_LOG("OpenChannel failed %d", static_cast<int>(errorCode));
        return FAILED;
    }

    return SUCCESS;
}

Result ObjectDetect::Init() {
    //If it is already initialized, it is returned
    if (isInited_) {
        INFO_LOG("Classify instance is initied already!");
        return SUCCESS;
    }
    //Initializes the ACL resource
    Result ret = InitResource();
    if (ret != SUCCESS) {
        ERROR_LOG("Init acl resource failed");
        return FAILED;
    }
    //Initializes the model management instance
    ret = InitModel(modelPath_);
    if (ret != SUCCESS) {
        ERROR_LOG("Init model failed");
        return FAILED;
    }
	
    ret = CreateModelInputdDataset();
    if (ret != SUCCESS) {
        ERROR_LOG("Create image info buf failed");
        return FAILED;
    }	
	
    /*
    //Connect the presenter server
    ret = OpenPresenterChannel();
    if (ret != SUCCESS) {
        ERROR_LOG("Open presenter channel failed");
        return FAILED;
    }
    */
    isInited_ = true;
    return SUCCESS;
}

Result ObjectDetect::Preprocess(cv::Mat& frame) {
    //Scale the frame image to the desired size of the model
    //2020.10.13这一步已经在主处理器端完成，只执行将图像格式转换的部分
    
    cv::Mat reiszeMat;
    cv::resize(frame, reiszeMat, cv::Size(modelWidth_, modelHeight_));
    if (reiszeMat.empty()) {
        ERROR_LOG("Resize image failed");
        return FAILED;
    }
    
    //Copy the data into the cache of the input dataset
    aclrtMemcpyKind policy = (runMode_ == ACL_HOST)?
                             ACL_MEMCPY_HOST_TO_DEVICE:ACL_MEMCPY_DEVICE_TO_DEVICE;
    aclError ret = aclrtMemcpy(imageDataBuf_, imageDataSize_,
                               reiszeMat.ptr<uint8_t>(), imageDataSize_, policy);
    if (ret != ACL_ERROR_NONE) {
        ERROR_LOG("Copy resized image data to device failed.");
        return FAILED;
    }

    return SUCCESS;
}

Result ObjectDetect::Inference(aclmdlDataset*& inferenceOutput) {
    //Perform reasoning
    Result ret = model_.Execute();
    if (ret != SUCCESS) {
        ERROR_LOG("Execute model inference failed");
        return FAILED;
    }
    //Get inference output
    inferenceOutput = model_.GetModelOutputData();

    return SUCCESS;
}

Result ObjectDetect::Postprocess(cv::Mat& frame,
                                    aclmdlDataset* modelOutput){
    //Get box information data
    uint32_t dataSize = 0;
    float* detectData = (float*)GetInferenceOutputItem(dataSize, modelOutput,
                                                       kBBoxDataBufId);
    if (detectData == nullptr) return FAILED;
    //Gets the number of boxes
    uint32_t* boxNum = (uint32_t*)GetInferenceOutputItem(dataSize, modelOutput,
                                                         kBoxNumDataBufId);
    if (boxNum == nullptr) return FAILED;

    //Number of boxes The first data is valid
    uint32_t totalBox = boxNum[0];
    //缩放因子
    float widthScale = (float)(frame.cols) / modelWidth_;
    float heightScale = (float)(frame.rows) / modelHeight_;

    //取消通过conf阈值直接处置，改为收集bbox情况后基于nms方法进行后处理
    vector<BoundingBox> bboxesOld,bboxesNew;
    //收集bbox
    for (uint32_t i = 0; i < totalBox; i++) {
        BoundingBox onebox;
        //boundbox
        //2020.10.14:注意,这里的顺序是xmin,ymin,xmax,ymax
        //可能更换为自己用的模型之后，这里输出又会变化
        onebox.lt_x=detectData[totalBox * TOPLEFTX + i] * widthScale;
        onebox.lt_y=detectData[totalBox * TOPLEFTY + i] * heightScale;
        onebox.rb_x=detectData[totalBox * BOTTOMRIGHTX + i] * widthScale;
        onebox.rb_y=detectData[totalBox * BOTTOMRIGHTY + i] * heightScale;
        onebox.attribute=(uint32_t)detectData[totalBox * LABEL + i];
        onebox.score=detectData[totalBox * SCORE + i];
        bboxesOld.push_back(onebox);
    }
    bboxesNew = nmsAllClasses(nmsThresh, bboxesOld,numClasses);

    for (uint32_t i = 0; i < bboxesNew.size(); i++) {
        //不可能出现的检测情况，出现则该检测结果认为无效
        //bbox高度、宽度不大于0
        if(bboxesNew[i].lt_y>=bboxesNew[i].rb_y || bboxesNew[i].lt_x>=bboxesNew[i].rb_x)
            continue;
        //可能会有bbox出界的情况，这时候需要修正
        if(bboxesNew[i].rb_y>frame.rows) bboxesNew[i].rb_y=frame.rows;
        if(bboxesNew[i].rb_x>frame.cols) bboxesNew[i].rb_x=frame.cols;

        shmdata=shmobj2.requiredata();//要求数据
        if(shmdata!=NULL)
        {
            BoundingBox temp=bboxesNew[i];
            memcpy(&shmdata[10],&temp,sizeof(BoundingBox));
            shmobj2.updatashm(shmdata);//更新数据，可被读取
        }
    }
    //这一帧最后一个待传输目标也已经发送，发送停止指令(bbox.attribute==88)
    BoundingBox finish_singal_bbox;
    finish_singal_bbox.attribute=88;
    shmdata=shmobj2.requiredata();//要求数据
    if(shmdata!=NULL)
    {
        memcpy(&shmdata[10],&finish_singal_bbox,sizeof(BoundingBox));
        shmobj2.updatashm(shmdata);//更新数据，可被读取
    }
    //If it is the host side, the data is copied from the device and the memory used by the copy is freed
    if (runMode_ == ACL_HOST) {
        delete[]((uint8_t*)detectData);
        delete[]((uint8_t*)boxNum);
    }

    //Sends inference results and images to presenter Server for display
    //SendImage(detectResults, frame);
    return SUCCESS;
}

void* ObjectDetect::GetInferenceOutputItem(uint32_t& itemDataSize,
                                           aclmdlDataset* inferenceOutput,
                                           uint32_t idx) {

    aclDataBuffer* dataBuffer = aclmdlGetDatasetBuffer(inferenceOutput, idx);
    if (dataBuffer == nullptr) {
        ERROR_LOG("Get the %dth dataset buffer from model "
        "inference output failed", idx);
        return nullptr;
    }

    void* dataBufferDev = aclGetDataBufferAddr(dataBuffer);
    if (dataBufferDev == nullptr) {
        ERROR_LOG("Get the %dth dataset buffer address "
        "from model inference output failed", idx);
        return nullptr;
    }

    size_t bufferSize = aclGetDataBufferSize(dataBuffer);
    if (bufferSize == 0) {
        ERROR_LOG("The %dth dataset buffer size of "
        "model inference output is 0", idx);
        return nullptr;
    }

    void* data = nullptr;
    if (runMode_ == ACL_HOST) {
        data = Utils::CopyDataDeviceToLocal(dataBufferDev, bufferSize);
        if (data == nullptr) {
            ERROR_LOG("Copy inference output to host failed");
            return nullptr;
        }
    }
    else {
        data = dataBufferDev;
    }

    itemDataSize = bufferSize;
    return data;
}

void ObjectDetect::EncodeImage(vector<uint8_t>& encodeImg,
                                  cv::Mat& origImg) {
    vector<int> param = vector<int>(2);
    param[0] = CV_IMWRITE_JPEG_QUALITY;
    param[1] = 95;//default(95) 0-100
    //Jpeg images must serialize the Proto message before they can be sent
    cv::imencode(".jpg", origImg, encodeImg, param);
}

Result ObjectDetect::SendImage(vector<DetectionResult>& detectionResults,
                                  cv::Mat& origImg) {
    vector<uint8_t> encodeImg;
    EncodeImage(encodeImg, origImg);

    ImageFrame imageParam;
    imageParam.format = ImageFormat::kJpeg;
    imageParam.width = origImg.cols;
    imageParam.height = origImg.rows;
    imageParam.size = encodeImg.size();
    imageParam.data = reinterpret_cast<uint8_t*>(encodeImg.data());
    imageParam.detection_results = detectionResults;
    //Sends the detected object frame information and frame image to the Presenter Server for display
    PresenterErrorCode errorCode = PresentImage(channel_, imageParam);
    if (errorCode != PresenterErrorCode::kNone) {
        ERROR_LOG("PresentImage failed %d", static_cast<int>(errorCode));
        return FAILED;
    }

    return SUCCESS;
}

void ObjectDetect::DestroyResource()
{
	aclrtFree(imageDataBuf_);
    aclrtFree(imageInfoBuf_);

    delete channel_;
	
    //The ACL resource held by the model instance must be released before the ACL exits or ABORT will be torn down
    model_.DestroyResource();

    aclError ret;
    ret = aclrtResetDevice(deviceId_);
    if (ret != ACL_ERROR_NONE) {
        ERROR_LOG("reset device failed");
    }
    INFO_LOG("end to reset device is %d", deviceId_);

    ret = aclFinalize();
    if (ret != ACL_ERROR_NONE) {
        ERROR_LOG("finalize acl failed");
    }
    INFO_LOG("end to finalize acl");

}

std::vector<BoundingBox> ObjectDetect::nonMaximumSuppression(const float nmsThresh, std::vector<BoundingBox> binfo)
{
    auto overlap1D = [](float x1min, float x1max, float x2min, float x2max) -> float {
        if (x1min > x2min)
        {
            std::swap(x1min, x2min);
            std::swap(x1max, x2max);
        }
        return x1max < x2min ? 0 : std::min(x1max, x2max) - x2min;
    };
    auto computeIoU = [&overlap1D](BoundingBox& bbox1, BoundingBox& bbox2) -> float {
        float overlapX = overlap1D(bbox1.lt_x, bbox1.rb_x, bbox2.lt_x, bbox2.rb_x);
        float overlapY = overlap1D(bbox1.lt_y, bbox1.rb_y, bbox2.lt_y, bbox2.rb_y);
        float area1 = (bbox1.rb_x - bbox1.lt_x) * (bbox1.rb_y - bbox1.lt_y);
        float area2 = (bbox2.rb_x - bbox2.lt_x) * (bbox2.rb_y - bbox2.lt_y);
        float overlap2D = overlapX * overlapY;
        float u = area1 + area2 - overlap2D;
        return u == 0 ? 0 : overlap2D / u;
    };

    std::stable_sort(binfo.begin(), binfo.end(),
                     [](const BoundingBox& b1, const BoundingBox& b2) { return b1.score > b2.score; });
    std::vector<BoundingBox> out;
    for (auto& i : binfo)
    {
        bool keep = true;
        for (auto& j : out)
        {
            if (keep)
            {
                float overlap = computeIoU(i, j);
                keep = overlap <= nmsThresh;
            }
            else
                break;
        }
        if (keep) out.push_back(i);
    }
    return out;
}

std::vector<BoundingBox> ObjectDetect::nmsAllClasses(const float nmsThresh, std::vector<BoundingBox>& binfo, const uint numClasses)
{
    std::vector<BoundingBox> result;
    std::vector<std::vector<BoundingBox>> splitBoxes(numClasses);
    for (auto& box : binfo)
    {
        splitBoxes.at(box.attribute).push_back(box);
    }

    for (auto& boxes : splitBoxes)
    {
        boxes = nonMaximumSuppression(nmsThresh, boxes);
        result.insert(result.end(), boxes.begin(), boxes.end());
    }

    return result;
}