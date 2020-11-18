#include <assert.h>
#include <cuda_runtime_api.h>
#include <sys/stat.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <malloc.h>
#include "NvInfer.h"
#include "NvOnnxParser.h"

#include <opencv2/opencv.hpp>
#include "image.h"
#include <sys/time.h>
#include <iterator>
#include <tensorrt_engine.h>
#include "thor/timer.h"
#include "thor/logging.h"
#include "thor/os.h"
#include "thor/structures.h"

#include "../include/batch_stream.h"
#include "../include/tensorrt_utils.h"
#include "../include/entropy_calibrator.h"

// we will using eigen so something
#include "eigen3/Eigen/Eigen"
#include "eigen3/Eigen/Core"


/**
 *
 * Inference on a new onnx converted trt model
 * using standalone TensorRT engine
 *
 */


using namespace thor::log;
using namespace nvinfer1;
using namespace Eigen;

static tensorrt::Logger gLogger;
std::stringstream gieModelStream;
static const int INPUT_H = 678;
static const int INPUT_W = 1024;
// once image size certain, priors_n also certain, how does this calculated?
static const int priors_n = 28672;

// BGR order
static const float kMeans[3] = {104.f, 117.f, 123.f};
// using for Priors
static const vector<vector<int>> min_sizes = {{16, 32}, {64, 128}, {256, 512}};
static const vector<int> steps = {8, 16, 32};
const char *INPUT_BLOB_NAME = "0";

struct Box {
  float x1;
  float y1;
  float x2;
  float y2;
};

struct Landmark {
  float x[5];
  float y[5];
};

struct FaceInfo {
  float score;
  Box box;
  Landmark landmark;
};

static int to_json_str(vector<FaceInfo> & face_info, char *face_json)
{
    int len;
    int i;
    int num_face = face_info.size();
    
    sprintf(face_json, "{num_face: %d", num_face);

   

    if (num_face > 0){
        len = strlen(face_json);
        sprintf(face_json + len, ", faces: [");

        for(i = 0 ; i < num_face ; ++i){
            len = strlen(face_json);

            if (i != 0){
                sprintf(face_json + len, ",");
                len += 1;
            }
            
            sprintf(face_json + len , "{"
                                         "score: %.3f, box:{x1:%.1f, y1:%.1f, x2:%.1f, y2:.1f},"
                                         "landmark: {x:[%.1f, %.1f, %.1f, %.1f], y:[%.1f, %.1f, %.1f, %.1f]}"
                                       "}",

                                        face_info[i].score, 
                                        face_info[i].box.x1, face_info[i].box.y1,face_info[i].box.x2,face_info[i].box.y2, 
                                        face_info[i].landmark.x[0], face_info[i].landmark.x[1], face_info[i].landmark.x[2], 
                                        face_info[i].landmark.x[3], face_info[i].landmark.x[4],
                                        face_info[i].landmark.y[0], face_info[i].landmark.y[1], face_info[i].landmark.y[2], 
                                        face_info[i].landmark.y[3], face_info[i].landmark.y[4]                                       
                                        

                );

            
        }

        
        len = strlen(face_json);
        sprintf(face_json + len, "faces: ]");

    }


    len = strlen(face_json);
    sprintf(face_json + len, "}");

    len = strlen(face_json);

    return len;

}

vector<Box> createPriors(vector<vector<int>> min_sizes, vector<int> steps, cv::Size img_size) {
  vector<Box> anchors;
  // 8, 16, 32
  for (int j = 0; j < steps.size(); ++j) {
    int step = steps[j];
    // featuremap sizes
    int fm_h = ceil(img_size.height * 1.0 / step);
    int fm_w = ceil(img_size.width * 1.0 / step);
    vector<int> min_sizes_k = min_sizes[j];
    // iter one featuremap
    for (int fi = 0; fi < fm_h; ++fi) {
      for (int fj = 0; fj < fm_w; ++fj) {
        for (int k = 0; k < min_sizes_k.size(); ++k) {
          int min_size = min_sizes_k[k];
          float s_kx = (float) min_size / img_size.width;
          float s_ky = (float) min_size / img_size.height;
          float cx = (float) ((fj + 0.5) * step) / img_size.width;
          float cy = (float) ((fi + 0.5) * step) / img_size.height;

          Box rect;
          rect.x1 = cx;
          rect.y1 = cy;
          rect.x2 = s_kx;
          rect.y2 = s_ky;
          anchors.emplace_back(rect);
        }
      }
    }
  }
  for (int kI = 0; kI < 5; ++kI) {
    LOG(INFO) << anchors[kI].x1 << " " << anchors[kI].y1 << " " << anchors[kI].x2 << " " << anchors[kI].y2;
  }
  return anchors;
}

bool CompareBBox(const FaceInfo &a, const FaceInfo &b) {
  return a.score > b.score;
}

std::vector<FaceInfo> nms(std::vector<FaceInfo> &bboxes,
                          float threshold) {
  std::vector<FaceInfo> bboxes_nms;
  std::sort(bboxes.begin(), bboxes.end(), CompareBBox);
  int32_t select_idx = 0;
  int32_t num_bbox = static_cast<int32_t>(bboxes.size());
  std::vector<int32_t> mask_merged(num_bbox, 0);
  bool all_merged = false;

  while (!all_merged) {
    while (select_idx < num_bbox && mask_merged[select_idx] == 1) select_idx++;
    if (select_idx == num_bbox) {
      all_merged = true;
      continue;
    }
    bboxes_nms.push_back(bboxes[select_idx]);
    mask_merged[select_idx] = 1;
    Box select_bbox = bboxes[select_idx].box;
    float area1 = static_cast<float>((select_bbox.x2 - select_bbox.x1 + 1) *
        (select_bbox.y2 - select_bbox.y1 + 1));
    float x1 = static_cast<float>(select_bbox.x1);
    float y1 = static_cast<float>(select_bbox.y1);
    float x2 = static_cast<float>(select_bbox.x2);
    float y2 = static_cast<float>(select_bbox.y2);

    select_idx++;
    for (int32_t i = select_idx; i < num_bbox; i++) {
      if (mask_merged[i] == 1) continue;

      Box &bbox_i = bboxes[i].box;
      float x = std::max<float>(x1, static_cast<float>(bbox_i.x1));
      float y = std::max<float>(y1, static_cast<float>(bbox_i.y1));
      float w = std::min<float>(x2, static_cast<float>(bbox_i.x2)) - x + 1;  //<- float 型不加1
      float h = std::min<float>(y2, static_cast<float>(bbox_i.y2)) - y + 1;
      if (w <= 0 || h <= 0) continue;

      float area2 = static_cast<float>((bbox_i.x2 - bbox_i.x1 + 1) *
          (bbox_i.y2 - bbox_i.y1 + 1));
      float area_intersect = w * h;

      if (static_cast<float>(area_intersect) /
          (area1 + area2 - area_intersect) >
          threshold) {
        mask_merged[i] = 1;
      }
    }
  }
  return bboxes_nms;
}

Box decodeBox(Box anchor, cv::Vec4f regress) {
  Box rect;
  rect.x1 = anchor.x1 + regress[0] * 0.1 * anchor.x2;
  rect.y1 = anchor.y1 + regress[1] * 0.1 * anchor.y2;
  rect.x2 = anchor.x2 * exp(regress[2] * 0.2);
  rect.y2 = anchor.y2 * exp(regress[3] * 0.2);
  rect.x1 -= (rect.x2 / 2);
  rect.y1 -= (rect.y2 / 2);
  rect.x2 += rect.x1;
  rect.y2 += rect.y1;
  return rect;
}

Landmark decodeLandmark(Box anchor, Landmark facePts) {
  Landmark landmark;
  for (int k = 0; k < 5; ++k) {
    landmark.x[k] = anchor.x1 + facePts.x[k] * 0.1 * anchor.x2;
    landmark.y[k] = anchor.y1 + facePts.y[k] * 0.1 * anchor.y2;
  }
  return landmark;
}

vector<FaceInfo> doPostProcess(float *out_box, float *out_landmark, float *out_conf,
    const vector<Box> &priors, float nms_threshold) {
  // 28672x4, 28672x2, 28672x10
  vector<FaceInfo> all_faces;
  for (int i = 0; i < priors_n; ++i) {
    // first column is background
    float conf_i = out_conf[2 * i + 1];
    if (conf_i >= 0.8) {
      // only score >= 0.5
      cv::Vec4f regress;
      float dx = out_box[4 * i];
      float dy = out_box[4 * i + 1];
      float dw = out_box[4 * i + 2];
      float dh = out_box[4 * i + 3];
      regress = cv::Vec4f(dx, dy, dw, dh);
      Box box = decodeBox(priors[i], regress);

      Landmark pts;
      for (size_t k = 0; k < 5; k++) {
        pts.x[k] = out_landmark[i * 10 + k * 2];
        pts.y[k] = out_landmark[i * 10 + k * 2 + 1];
      }
      Landmark landmark = decodeLandmark(priors[i], pts);
      FaceInfo one_face;
      one_face.box = box;
      one_face.score = conf_i;
      one_face.landmark = landmark;
      all_faces.emplace_back(one_face);
    }
  }
  // do nms here
  all_faces = nms(all_faces, nms_threshold);
  return all_faces;
}

vector<FaceInfo> doInference(IExecutionContext &context, float *input, const vector<Box> &priors, int batchSize,
                             float nms_threshold = 0.86) {
  const ICudaEngine &engine = context.getEngine();
  // we have 4 bindings for retinaface
  assert(engine.getNbBindings() == 4);

  void *buffers[4];
  std::vector<int64_t> bufferSize;
  int nbBindings = engine.getNbBindings();
  bufferSize.resize(nbBindings);

  for (int kI = 0; kI < nbBindings; ++kI) {
    nvinfer1::Dims dims = engine.getBindingDimensions(kI);
    nvinfer1::DataType dt = engine.getBindingDataType(kI);
    int64_t totalSize = volume(dims) * 1 * getElementSize(dt);
    bufferSize[kI] = totalSize;
//    LOG(INFO) << "binding " << kI << " nodeName: " << engine.getBindingName(kI) << " total size: " << totalSize;
    CHECK(cudaMalloc(&buffers[kI], totalSize));
  }

  auto out1 = new float[bufferSize[1] / sizeof(float)];
  auto out2 = new float[bufferSize[2] / sizeof(float)];
  auto out3 = new float[bufferSize[3] / sizeof(float)];

  cudaStream_t stream;
  CHECK(cudaStreamCreate(&stream));
  CHECK(cudaMemcpyAsync(buffers[0], input, bufferSize[0], cudaMemcpyHostToDevice, stream));
//  context.enqueue(batchSize, buffers, stream,nullptr);
  context.enqueue(1, buffers, stream, nullptr);

  CHECK(cudaMemcpyAsync(out1, buffers[1], bufferSize[1], cudaMemcpyDeviceToHost, stream));
  CHECK(cudaMemcpyAsync(out2, buffers[2], bufferSize[2], cudaMemcpyDeviceToHost, stream));
  CHECK(cudaMemcpyAsync(out3, buffers[3], bufferSize[3], cudaMemcpyDeviceToHost, stream));
  cudaStreamSynchronize(stream);

  // release the stream and the buffers
  cudaStreamDestroy(stream);
  CHECK(cudaFree(buffers[0]));
  CHECK(cudaFree(buffers[1]));
  CHECK(cudaFree(buffers[2]));
  CHECK(cudaFree(buffers[3]));
  // box, landmark, conf
  // 28672x4, 28672x2, 28672x10
  // out1: 4 box, out2: 2 conf, out3: 10 landmark
  vector<FaceInfo> all_faces = doPostProcess(out1, out2, out3, priors, nms_threshold);
  return all_faces;
}


#define MAX_FACE            (1024)

typedef struct 
{
    int                         img_w;
    int                         img_h;
    tensorrt::TensorRTEngine    *h_trt_engine;
    ICudaEngine                 *h_cuda_engine;
    IExecutionContext           *exe_context;
    vector<Box>                 priors;
    char                        face_out_str[16384];
}TRTEngineRetinaface_Context;


#include <Python.h>
#include <opencv2/highgui/highgui.hpp>
#include "pyboostcvconverter/pyboostcvconverter.hpp"
using namespace cv;

static void * trt_engine_retinaface_create(const char * engine_path, int img_w, int img_h)
{
    TRTEngineRetinaface_Context * ctx;

    ctx = new TRTEngineRetinaface_Context;
    ctx->h_cuda_engine = NULL;
    ctx->h_trt_engine = NULL;
    ctx->exe_context = NULL;
    ctx->img_w = img_w;
    ctx->img_h = img_h;

    ctx->h_trt_engine = new tensorrt::TensorRTEngine(engine_path);
    
    ctx->h_cuda_engine = ctx->h_trt_engine->getEngine();
    if (!ctx->h_cuda_engine){
        goto err_out;
    }
    
    CheckEngine(ctx->h_cuda_engine);

    ctx->exe_context = ctx->h_cuda_engine->createExecutionContext();

    if (!ctx->exe_context){
        goto err_out;
    }

    ctx->priors = createPriors(min_sizes, steps, cv::Size(img_h, img_w));


    return (void *)ctx;
    
err_out:

    if (ctx->exe_context){
        delete ctx->exe_context;
        ctx->exe_context = NULL;
    }
    
    if (ctx->h_cuda_engine){
        delete ctx->h_cuda_engine;
        ctx->h_cuda_engine = NULL;
    }
    
    if (ctx->h_trt_engine){
        delete ctx->h_trt_engine;
        ctx->h_trt_engine = NULL;
    }

    delete ctx;

    return NULL;
    
}


static const char * trt_engine_retinaface_detect(void *h_engine, cv::Mat &img)
{
    TRTEngineRetinaface_Context *ctx;
    float *data;
    int size;
    int len;
    
    ctx = (TRTEngineRetinaface_Context *)h_engine;
    
    cv::Mat resizedImage = cv::Mat::zeros(ctx->img_h, ctx->img_w, CV_32FC3);
    
    cv::resize(img, resizedImage, cv::Size(ctx->img_w, ctx->img_h));
    data = HWC2CHW(resizedImage, kMeans);


    vector<FaceInfo> all_faces = doInference(*ctx->exe_context, data, ctx->priors, 1);

    len = to_json_str(all_faces, ctx->face_out_str);

    return ctx->face_out_str;
}

void trt_engine_retinaface_destroy(void *h_engine)
{
    TRTEngineRetinaface_Context   *ctx;

    ctx = (TRTEngineRetinaface_Context *)h_engine;
    
    if (ctx->exe_context){
        delete ctx->exe_context;
        ctx->exe_context = NULL;
    }

    if (ctx->h_cuda_engine){
        delete ctx->h_cuda_engine;
        ctx->h_cuda_engine = NULL;
    }

    if (ctx->h_trt_engine){
        delete ctx->h_trt_engine;
        ctx->h_trt_engine = NULL;
    }

    delete ctx;
}

static PyObject * pyCreate(PyObject *self, PyObject *args)
{
    char *engine_name;
    int w, h;
    void *ctx;

    
    if (!PyArg_ParseTuple(args, "Sii", &engine_name, &w, &h))
        return NULL;

    ctx = trt_engine_retinaface_create(engine_name, w, h);

    return Py_BuildValue("O&", ctx);

}

static PyObject *pyDetect(PyObject *self, PyObject *args)
{
    void *engine;
    PyObject *ndArray;
    const char *ret;
    

    if (!PyArg_ParseTuple(args, "O&O", &engine, &ndArray))
        return NULL;
    Mat mat = pbcvt::fromNDArrayToMat(ndArray);

    
    ret = trt_engine_retinaface_detect(engine, mat);

    return Py_BuildValue("s", ret);
}

static void PyDestroy(PyObject *self, PyObject *args)
{
    void *engine;
    
    if (!PyArg_ParseTuple(args, "O&", &engine))
        return NULL;
    
    trt_engine_retinaface_destroy(engine);

}

static PyMethodDef TRTRetinaFaceMeThods[] = {
    {"create", pyCreate, METH_VARARGS, "Create the engine."},
    {"detect", pyDetect, METH_VARARGS, "use the engine to detect image"},    
    {"destroy", pyDetect, METH_VARARGS, "destroy the engine"},        
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef TRTRetinaFaceModule = {
    PyModuleDef_HEAD_INIT,
    "TRTRetinaFace",     /* name of module */
    "",          /* module documentation, may be NULL */
    -1,          /* size of per-interpreter state of the module, or -1 if the module keeps state in global variables. */
    TRTRetinaFaceMeThods
};

PyMODINIT_FUNC PyInit_TRTRetinaFace(void) {
    return PyModule_Create(&TRTRetinaFaceModule);
}



