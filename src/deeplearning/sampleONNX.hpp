#include <iostream>
#include <fstream>
#include <NvInfer.h>
#include <memory>
#include <NvOnnxParser.h>
#include <vector>
#include <cuda_runtime_api.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/core.hpp>
#include <algorithm>
#include <numeric>
#include "NvUffParser.h"

// utilities ----------------------------------------------------------------------------------------------------------
// class to log errors, warnings, and other information during the build and inference phases
namespace sampleonnx{
    class Logger : public nvinfer1::ILogger
    {
    public:
        void log(Severity severity, const char* msg) override {
            // remove this 'if' if you need more logged info
            if ((severity == Severity::kERROR) || (severity == Severity::kINTERNAL_ERROR)) {
                std::cout << msg << "\n";
            }
        }
    } gLogger;

    // destroy TensorRT objects if something goes wrong
    struct TRTDestroy
    {
        template <class T>
        void operator()(T* obj) const
        {
            if (obj)
            {
                obj->destroy();
            }
        }
    };

    template <class T>
    using TRTUniquePtr = std::unique_ptr<T, TRTDestroy>;

    // calculate size of tensor
    size_t getSizeByDim(const nvinfer1::Dims& dims)
    {
        size_t size = 1;
        for (size_t i = 0; i < dims.nbDims; ++i)
        {
            size *= dims.d[i];
        }
        return size;
    }

    // get classes names
    std::vector<std::string> getClassNames(const std::string& imagenet_classes)
    {
        std::ifstream classes_file(imagenet_classes);
        std::vector<std::string> classes;
        if (!classes_file.good())
        {
            std::cerr << "ERROR: can't read file with classes names.\n";
            return classes;
        }
        std::string class_name;
        while (std::getline(classes_file, class_name))
        {
            classes.push_back(class_name);
        }
        return classes;
    }

    // preprocessing stage ------------------------------------------------------------------------------------------------
    void preprocessImage(const std::string& image_path, float* gpu_input, const nvinfer1::Dims& dims)
    {
        // read input image
        cv::Mat frame = cv::imread(image_path,cv::IMREAD_COLOR);
        if (frame.empty())
        {
            std::cerr << "Input image " << image_path << " load failed\n";
            return;
        }
        auto input_width = dims.d[3];
        auto input_height = dims.d[2];
        auto channels = dims.d[1];
        auto input_size = cv::Size(input_width, input_height);
        // resize
        cv::Mat resized;
        cv::resize(frame,resized,input_size, 0, 0, cv::INTER_NEAREST);
        // normalize
        cv::Mat flt_image;
        resized.convertTo(flt_image, CV_32FC3, 1.f / 255.f);
        cv::subtract(flt_image, cv::Scalar(0.485f, 0.456f, 0.406f), flt_image, cv::noArray(), -1);
        cv::divide(flt_image, cv::Scalar(0.229f, 0.224f, 0.225f), flt_image, 1, -1);
        // to tensor
        std::vector<cv::Mat> chw;
        
        cv::split(flt_image, chw);
        for (size_t i = 0; i < channels; ++i)
        {
            cudaMemcpy(gpu_input,chw[i].data,i * input_width * input_height*sizeof(float),cudaMemcpyHostToDevice);
            //chw.emplace_back(cv::Mat(input_size, CV_32FC1, gpu_input + i * input_width * input_height));
        }
        
    }

    // post-processing stage ----------------------------------------------------------------------------------------------
    void postprocessResults(float *gpu_output, const nvinfer1::Dims &dims, int batch_size)
    {
        // get class names
        auto classes = getClassNames("/home/acanus/github/models/imagenet_classes.txt");

        // copy results from GPU to CPU
        std::vector<float> cpu_output(getSizeByDim(dims) * batch_size);
        cudaMemcpy(cpu_output.data(), gpu_output, cpu_output.size() * sizeof(float), cudaMemcpyDeviceToHost);

        // calculate softmax
        std::transform(cpu_output.begin(), cpu_output.end(), cpu_output.begin(), [](float val) {return std::exp(val);});
        auto sum = std::accumulate(cpu_output.begin(), cpu_output.end(), 0.0);
        // find top classes predicted by the model
        std::vector<int> indices(getSizeByDim(dims) * batch_size);
        std::iota(indices.begin(), indices.end(), 0); // generate sequence 0, 1, 2, 3, ..., 999
        std::sort(indices.begin(), indices.end(), [&cpu_output](int i1, int i2) {return cpu_output[i1] > cpu_output[i2];});
        // print results
        int i = 0;
        while (cpu_output[indices[i]] / sum > 0.005)
        {
            if (classes.size() > indices[i])
            {
                std::cout << "class: " << classes[indices[i]] << " | ";
            }
            std::cout << "confidence: " << 100 * cpu_output[indices[i]] / sum << "% | index: " << indices[i] << "\n";
            ++i;
        }
    }

    // initialize TensorRT engine and parse ONNX model --------------------------------------------------------------------
    void parseOnnxModel(const std::string& model_path, TRTUniquePtr<nvinfer1::ICudaEngine>& engine,
                        TRTUniquePtr<nvinfer1::IExecutionContext>& context)
    {
        TRTUniquePtr<nvinfer1::IBuilder> builder{nvinfer1::createInferBuilder(gLogger)};
        const auto explicitBatch = 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
        nvinfer1::INetworkDefinition *network = builder->createNetworkV2(explicitBatch);
        TRTUniquePtr<nvonnxparser::IParser> parser{nvonnxparser::createParser(*network, gLogger)};
        TRTUniquePtr<nvinfer1::IBuilderConfig> config{builder->createBuilderConfig()};
        
        // parse ONNX
        if (!parser->parseFromFile(model_path.c_str(), static_cast<int>(nvinfer1::ILogger::Severity::kINFO)))
        {
            std::cerr << "ERROR: could not parse the model.\n";
            return;
        }
        IOptimizationProfile* profile = builder->createOptimizationProfile();
        profile->setDimensions("input_tensor:0", OptProfileSelector::kMIN, Dims4(1,3,224,224));
        profile->setDimensions("input_tensor:0", OptProfileSelector::kOPT, Dims4(1,3,224,224));
        profile->setDimensions("input_tensor:0", OptProfileSelector::kMAX, Dims4(1,3,224,224));

        config->addOptimizationProfile(profile);
        // allow TensorRT to use up to 1GB of GPU memory for tactic selection.
        config->setMaxWorkspaceSize(1ULL << 30);
        // use FP16 mode if possible
        if (builder->platformHasFastFp16())
        {
            config->setFlag(nvinfer1::BuilderFlag::kFP16);
        }
        // we have only one image in batch
        builder->setMaxBatchSize(1);
        // generate TensorRT engine optimized for the target platform
        engine.reset(builder->buildEngineWithConfig(*network, *config));
        context.reset(engine->createExecutionContext());
    }

    // ON GOING ------------------------------------------------------------------------------------------------------
    // initialize TensorRT engine and parse Uff model --------------------------------------------------------------------
    void parseUffModel(const std::string& model_path, TRTUniquePtr<nvinfer1::ICudaEngine>& engine,
                        TRTUniquePtr<nvinfer1::IExecutionContext>& context)
    {
        TRTUniquePtr<nvinfer1::IBuilder> builder{nvinfer1::createInferBuilder(gLogger)};
        const auto explicitBatch = 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
        nvinfer1::INetworkDefinition *network = builder->createNetworkV2(explicitBatch);
        TRTUniquePtr<nvuffparser::IUffParser> parser{nvuffparser::createUffParser()};
        TRTUniquePtr<nvinfer1::IBuilderConfig> config{builder->createBuilderConfig()};
        
        // parse Uff
        
        parser->registerInput(model_path.c_str(), nvinfer1::Dims3(1, 28, 28), nvuffparser::UffInputOrder::kNCHW);
        //parser->registerOutput(mParams.outputTensorNames[0].c_str());

        //parser->parse(mParams.uffFileName.c_str(), *network, nvinfer1::DataType::kFLOAT);

        IOptimizationProfile* profile = builder->createOptimizationProfile();
        profile->setDimensions("input_tensor:0", OptProfileSelector::kMIN, Dims4(1,3,224,224));
        profile->setDimensions("input_tensor:0", OptProfileSelector::kOPT, Dims4(1,3,224,224));
        profile->setDimensions("input_tensor:0", OptProfileSelector::kMAX, Dims4(1,3,224,224));
        config->addOptimizationProfile(profile);
        // allow TensorRT to use up to 1GB of GPU memory for tactic selection.
        config->setMaxWorkspaceSize(1ULL << 30);
        // use FP16 mode if possible
        if (builder->platformHasFastFp16())
        {
            config->setFlag(nvinfer1::BuilderFlag::kFP16);
        }
        // we have only one image in batch
        builder->setMaxBatchSize(1);
        
        // generate TensorRT engine optimized for the target platform
        engine.reset(builder->buildEngineWithConfig(*network, *config));
        context.reset(engine->createExecutionContext());
    }

    // main pipeline ------------------------------------------------------------------------------------------------------
    void test()
    {
        std::string model_path="/home/acanus/github/models/updated_model.onnx";
        std::string image_path="/home/acanus/github/images/turkish_coffee.jpg";
        int batch_size = 1;

        // initialize TensorRT engine and parse ONNX model
        TRTUniquePtr<nvinfer1::ICudaEngine> engine{nullptr};
        TRTUniquePtr<nvinfer1::IExecutionContext> context{nullptr};
        parseOnnxModel(model_path, engine, context);

        // get sizes of input and output and allocate memory required for input data and for output data
        std::vector<nvinfer1::Dims> input_dims; // we expect only one input
        std::vector<nvinfer1::Dims> output_dims; // and one output
        std::vector<void*> buffers(engine->getNbBindings()); // buffers for input and output data
        for (size_t i = 0; i < engine->getNbBindings(); ++i)
        {
            auto binding_size = getSizeByDim(engine->getBindingDimensions(i)) * batch_size * sizeof(float);
            cudaMalloc(&buffers[i], binding_size);
            if (engine->bindingIsInput(i))
            {
                input_dims.emplace_back(engine->getBindingDimensions(i));
            }
            else
            {
                output_dims.emplace_back(engine->getBindingDimensions(i));
            }
        }
        if (input_dims.empty() || output_dims.empty())
        {
            std::cerr << "Expect at least one input and one output for network\n";
            return ;
        }

        // preprocess input data
        preprocessImage(image_path, (float *) buffers[0], input_dims[0]);
        // inference
        auto start_inner = high_resolution_clock::now();
        context->enqueue(batch_size, buffers.data(), 0, nullptr);
        auto stop_inner = duration_cast<milliseconds> (high_resolution_clock::now() - start_inner);
        cout << "inference time : " << stop_inner.count() << "ms" << endl;
        // postprocess results
        postprocessResults((float *) buffers[1], output_dims[0], batch_size);

        for (void* buf : buffers)
        {
            cudaFree(buf);
        }
    }
}
