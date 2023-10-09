/*
 * Copyright (c) 2021. Huawei Technologies Co., Ltd. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*mxbase cpp */
#include "Alphapose.h"

#include <dirent.h>
#include <sys/stat.h>
#include <fstream>
#include <iostream>

#include <boost/property_tree/json_parser.hpp>
#include <opencv4/opencv2/core.hpp>
#include <opencv4/opencv2/opencv.hpp>

#include "acl/acl.h"
#include "MxBase/DeviceManager/DeviceManager.h"
#include "MxBase/Log/Log.h"
#include "MxBase/CV/ObjectDetection/Nms/Nms.h"

namespace {
const uint32_t YUV_BYTE_NU = 3;
const uint32_t YUV_BYTE_DE = 2;

const uint32_t MODEL_HEIGHT = 256;
const uint32_t MODEL_WIDTH = 192;
}  // namespace

void PrintTensorShape(const std::vector<MxBase::TensorDesc> &tensorDescVec,
                      const std::string &tensorName) {
    LogInfo << "The shape of " << tensorName << " is as follows:";
    for (size_t i = 0; i < tensorDescVec.size(); ++i) {
        LogInfo << "  Tensor " << i << ":";
        for (size_t j = 0; j < tensorDescVec[i].tensorDims.size(); ++j) {
            LogInfo << "   dim: " << j << ": " << tensorDescVec[i].tensorDims[j];
        }
    }
}

APP_ERROR Alphapose::Init(const InitParam &initParam) {
    deviceId_ = initParam.deviceId;
    APP_ERROR ret = MxBase::DeviceManager::GetInstance()->InitDevices();
    if (ret != APP_ERR_OK) {
        LogError << "Init devices failed, ret=" << ret << ".";
        return ret;
    }
    ret = MxBase::TensorContext::GetInstance()->SetContext(initParam.deviceId);
    if (ret != APP_ERR_OK) {
        LogError << "Set context failed, ret=" << ret << ".";
        return ret;
    }
    dvppWrapper_ = std::make_shared<MxBase::DvppWrapper>();
    ret = dvppWrapper_->Init();
    if (ret != APP_ERR_OK) {
        LogError << "DvppWrapper init failed, ret=" << ret << ".";
        return ret;
    }
    model_ = std::make_shared<MxBase::ModelInferenceProcessor>();
    ret = model_->Init(initParam.modelPath, modelDesc_);
    if (ret != APP_ERR_OK) {
        LogError << "ModelInferenceProcessor init failed, ret=" << ret << ".";
        return ret;
    }

    PrintTensorShape(modelDesc_.inputTensors, "Model Input Tensors");
    PrintTensorShape(modelDesc_.outputTensors, "Model Output Tensors");

    return APP_ERR_OK;
}

APP_ERROR Alphapose::DeInit() {
    dvppWrapper_->DeInit();
    model_->DeInit();
    MxBase::DeviceManager::GetInstance()->DestroyDevices();
    return APP_ERR_OK;
}

APP_ERROR Alphapose::Inference(const std::vector<MxBase::TensorBase> &inputs,
                               std::vector<MxBase::TensorBase> *outputs) {
    auto dtypes = model_->GetOutputDataType();
    for (size_t i = 0; i < modelDesc_.outputTensors.size(); ++i) {
        std::vector<uint32_t> shape = {};
        for (size_t j = 0; j < modelDesc_.outputTensors[i].tensorDims.size(); ++j) {
            shape.push_back((uint32_t)modelDesc_.outputTensors[i].tensorDims[j]);
        }
        MxBase::TensorBase tensor(shape, dtypes[i],
                                  MxBase::MemoryData::MemoryType::MEMORY_DEVICE,
                                  deviceId_);
        APP_ERROR ret = MxBase::TensorBase::TensorBaseMalloc(tensor);
        if (ret != APP_ERR_OK) {
            LogError << "TensorBaseMalloc failed, ret=" << ret << ".";
            return ret;
        }
        outputs->push_back(tensor);
    }
    MxBase::DynamicInfo dynamicInfo = {};
    dynamicInfo.dynamicType = MxBase::DynamicType::STATIC_BATCH;
    auto startTime = std::chrono::high_resolution_clock::now();
    APP_ERROR ret = model_->ModelInference(inputs, *outputs, dynamicInfo);
    auto endTime = std::chrono::high_resolution_clock::now();
    double costMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();  // save time
    inferCostTimeMilliSec += costMs;
    if (ret != APP_ERR_OK) {
        LogError << "ModelInference failed, ret=" << ret << ".";
        return ret;
    }
    return APP_ERR_OK;
}

APP_ERROR Alphapose::Process(const std::string &imgPath,
                             const std::string &resultPath,
                             const std::string &dataset_name) {
    ImageShape imageShape{};
    ImageShape resizedImageShape{};
    std::vector<std::string> dirs;
    std::string data_set;
    if (dataset_name == "TUM") {
        data_set = imgPath + "/TUM/rgbd_dataset_freiburg2_desk_with_person";
        dirs.emplace_back("TUM");
    } else if (dataset_name == "Kitti") {
        data_set = imgPath + "/Kitti_raw_data/";
        dirs = GetAlldir(data_set, dataset_name);
    } else if (dataset_name == "Sintel") {
        data_set = imgPath + "/Sintel/final_left/";
        dirs = GetAlldir(data_set, dataset_name);
    } else {
        data_set = imgPath;
        dirs = GetAlldir(data_set, dataset_name);
        std::cout << "other dataset start" << std::endl;
    }
    for (const auto &dir : dirs) {
        std::vector<std::string> images;
        if (dataset_name == "TUM")
            images = GetAlldir(data_set, "TUM");
        else
            images = GetAllFiles(data_set + dir);
        for (const auto& image_file : images) {
            float fnum[3 * 256 * 192] = {0};
            std::ifstream in(image_file, std::ios::in | std::ios::binary);
            LogInfo << imgPath;
            in.read(reinterpret_cast<char*>(&fnum), sizeof (fnum));

            MxBase::TensorBase tensorBase;
            const uint32_t dataSize = 1 * 3 * 256 * 192 * 4;
            MxBase::MemoryData memoryDataDst(dataSize, MxBase::MemoryData::MEMORY_DEVICE, deviceId_);
            MxBase::MemoryData memoryDataSrc(reinterpret_cast<void*>(fnum), dataSize,
                                     MxBase::MemoryData::MEMORY_HOST_MALLOC);
            APP_ERROR ret = MxBase::MemoryHelper::MxbsMallocAndCopy(memoryDataDst, memoryDataSrc);

            if (ret != APP_ERR_OK) {
                LogError << GetError(ret) << "Memory malloc failed.";
                return ret;
            }
            std::vector<uint32_t> shape = {static_cast<uint32_t>(1), static_cast<uint32_t>(3),
                                           static_cast<uint32_t>(256), static_cast<uint32_t>(192)};
            tensorBase = MxBase::TensorBase(memoryDataDst, false, shape, MxBase::TENSOR_DTYPE_FLOAT32);

            std::vector<MxBase::TensorBase> inputs = {};
            std::vector<MxBase::TensorBase> outputs = {};
            inputs.push_back(tensorBase);

            std::cout << "inputs shape is " << inputs[0].GetShape()[0] << " "
                      << inputs[0].GetShape()[1] << " " << inputs[0].GetShape()[2]
                      << " " << inputs[0].GetShape()[3] << std::endl;

            auto startTime = std::chrono::high_resolution_clock::now();
            ret = Inference(inputs, &outputs);
            auto endTime = std::chrono::high_resolution_clock::now();
            double costMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();  // save time
            inferCostTimeMilliSec += costMs;
            if (ret != APP_ERR_OK) {
                LogError << "Inference failed, ret=" << ret << ".";
                return ret;
            }
            WriteResult(image_file, &outputs, dataset_name, dir);
            std::cout << "time is " << costMs << std::endl;
        }
    }
    return APP_ERR_OK;
}

std::vector<std::string> GetAlldir(const std::string& dir_name,
                                   const std::string& data_name) {
    std::vector<std::string> res;
    if (data_name == "TUM") {
        std::string txt_name = dir_name + "/associate.txt";
        std::ifstream infile;
        infile.open(txt_name.data());
        std::string s;
        while (getline(infile, s)) {
            std::stringstream input(s);
            std::string result;
            input >> result;
            res.emplace_back(dir_name + "/" + result);
        }
        return res;
    }
    struct dirent *filename;
    DIR *dir = OpenDir(dir_name);
    if (dir == nullptr) {
        return {};
    }

    if (data_name == "Kitti") {
        while ((filename = readdir(dir)) != nullptr) {
            std::string d_name = std::string(filename->d_name);
            // get rid of "." and ".."
            if (d_name == "." || d_name == ".." || filename->d_type != DT_DIR)
                continue;
            res.emplace_back(d_name + "/image");
            std::cout << "image_file name is " << d_name << std::endl;
        }
    } else {
        while ((filename = readdir(dir)) != nullptr) {
            std::string d_name = std::string(filename->d_name);
            // get rid of "." and ".."
            if (d_name == "." || d_name == ".." || filename->d_type != DT_DIR)
                continue;
            res.emplace_back(d_name);
        }
    }

    return res;
}
/*
APP_ERROR Alphapose::GetMetaMap(const ImageShape imgShape,
                                const ImageShape resizeimgShape, 
                                std::map<std::string, float> &meta) {
    std::vector<float> c = {static_cast<float>(imgShape.width) / 2, 
                            static_cast<float>(imgShape.height) / 2};
    float s = std::max(static_cast<float>(resizeimgShape.width) / 
                       static_cast<float>(resizeimgShape.height)
                       * static_cast<float>(imgShape.height), 
                       static_cast<float>(imgShape.width)) * 1.0;
    meta["c1"] = c[0];
    meta["c2"] = c[1];
    meta["s"] = s;
    meta["out_height"] = resizeimgShape.height / 4;
    meta["out_width"] = resizeimgShape.width / 4;
    return APP_ERR_OK;
}*/

std::vector<std::string> GetAllFiles(std::string dirName) {
    struct dirent *filename;
    DIR *dir = OpenDir(dirName);
    std::cout << dirName << std::endl;
    if (dir == nullptr) {
        return {};
    }
    std::vector<std::string> res;
    while ((filename = readdir(dir)) != nullptr) {
        std::string dName = std::string(filename->d_name);
        if (dName == "." || dName == ".." || filename->d_type != DT_REG) {
            continue;
        }
        res.emplace_back(std::string(dirName) + "/" + filename->d_name);
    }
    std::sort(res.begin(), res.end());
    for (auto &f : res) {
        std::cout << "image file: " << f << std::endl;
    }
    return res;
}

std::string RealPath(std::string path) {
    char realPathMem[PATH_MAX] = {0};
    char *realPathRet = nullptr;
    realPathRet = realpath(path.data(), realPathMem);
    if (realPathRet == nullptr) {
        std::cout << "File: " << path << " is not exist.";
        return "";
    }

    std::string realPath(realPathMem);
    std::cout << path << " realpath is: " << realPath << std::endl;
    return realPath;
}

DIR *OpenDir(std::string dirName) {
    if (dirName.empty()) {
        std::cout << " dirName is null ! " << std::endl;
        return nullptr;
    }
    std::string realPath = RealPath(dirName);
    struct stat s;
    lstat(realPath.c_str(), &s);
    if (!S_ISDIR(s.st_mode)) {
        std::cout << "dirName is not a valid directory !" << std::endl;
        return nullptr;
    }
    DIR *dir = opendir(realPath.c_str());
    if (dir == nullptr) {
        std::cout << "Can not open dir " << dirName << std::endl;
        return nullptr;
    }
    std::cout << "Successfully opened the dir " << dirName << std::endl;
    return dir;
}

APP_ERROR  Alphapose::WriteResult(const std::string& imageFile,
                                  std::vector<MxBase::TensorBase> *outputs,
                                  const std::string & dataset_name,
                                  const std::string& seq) {
    std::string homePath;
    if (dataset_name == "Kitti") {
        int pos = seq.find('/');
        std::string seq1(seq, 0, pos);
        homePath = "./final_result_Files/" + dataset_name + "/" + seq1;
        std::cout << "1" << std::endl;
    } else {
        homePath = "./final_result_Files/" + dataset_name + "/" + seq;
    }
    std::string path1 = "mkdir -p " + homePath;
    system(path1.c_str());
    std::cout << "homePath is " << homePath << std::endl;
    for (size_t i = 0; i < (*outputs).size(); ++i) {
        size_t outputSize;
        APP_ERROR ret = (*outputs)[i].ToHost();
        if (ret != APP_ERR_OK) {
            LogError << GetError(ret) << "tohost fail.";
            return ret;
        }
        void *netOutput = (*outputs)[i].GetBuffer();

        std::vector<uint32_t> out_shape = (*outputs)[i].GetShape();
        LogDebug << "shape is " << out_shape[0] << " " << out_shape[1] << " "
                                << out_shape[2] << " " << out_shape[3] << std::endl;
        outputSize =(*outputs)[i].GetByteSize();
        std::cout << "outputsize is " << outputSize << std::endl;
        int pos = imageFile.rfind('/');
        std::string fileName(imageFile, pos + 1);
        fileName.replace(fileName.rfind('.'), fileName.size() - fileName.rfind('.'), '_' + std::to_string(i) + ".bin");
        std::string outFileName = homePath + "/" + fileName;
        std::cout << "output file is " << outFileName << std::endl;
        FILE *outputFile = fopen(outFileName.c_str(), "wb");
        auto count1 = fwrite(netOutput, outputSize, sizeof(char), outputFile);
        std::cout << "count is " << count1 << " " << sizeof(char) << std::endl;
        fclose(outputFile);
        outputFile = nullptr;
    }

    LogDebug << "AlphaposeMindspore write results succeeded.";
    return APP_ERR_OK;
}
