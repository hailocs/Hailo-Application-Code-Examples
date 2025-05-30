#include "async_inference.hpp"
#include "utils.hpp"

#if defined(__unix__)
#include <sys/mman.h>
#endif


static std::shared_ptr<uint8_t> page_aligned_alloc(size_t size, void* buff = nullptr) {
    #if defined(__unix__)
        auto addr = mmap(buff, size, PROT_WRITE | PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (MAP_FAILED == addr) throw std::bad_alloc();
        return std::shared_ptr<uint8_t>(reinterpret_cast<uint8_t*>(addr), [size](void *addr) { munmap(addr, size); });
    #elif defined(_MSC_VER)
        auto addr = VirtualAlloc(buff, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!addr) throw std::bad_alloc();
        return std::shared_ptr<uint8_t>(reinterpret_cast<uint8_t*>(addr), [](void *addr){ VirtualFree(addr, 0, MEM_RELEASE); });
    #else
    #pragma error("Aligned alloc not supported")
    #endif
}

AsyncModelInfer::AsyncModelInfer(const std::string &hef_path)
{
    
    this->vdevice = hailort::VDevice::create().expect("Failed to create VDevice");

    auto infer_model_exp = this->vdevice->create_infer_model(hef_path);
    if (!infer_model_exp) {
        std::cerr << "Failed to create infer model, status = " << infer_model_exp.status() << std::endl;
        throw std::runtime_error("Failed to create infer model");
    }
    this->infer_model = infer_model_exp.release();

    this->input_buffer_guards.reserve(this->infer_model->inputs().size());
    this->output_buffer_guards.reserve(this->infer_model->outputs().size());
    for (auto& output_vstream_info : this->infer_model->hef().get_output_vstream_infos().release()) {
        std::string name(output_vstream_info.name);
        this->output_vstream_info_by_name[name] = output_vstream_info;
    }
    this->configured_infer_model = this->infer_model->configure().expect("Failed to create configured infer model");
    this->bindings = configured_infer_model.create_bindings().expect("Failed to create infer bindings");
}

AsyncModelInfer::AsyncModelInfer(const std::string &hef_path,const std::string &group_id) 
{   
    hailo_vdevice_params_t vdevice_params = {0};
    hailo_init_vdevice_params(&vdevice_params);
    vdevice_params.group_id = group_id.c_str();
    this->vdevice = hailort::VDevice::create(vdevice_params).expect("Failed to create VDevice");

    auto infer_model_exp = vdevice->create_infer_model(hef_path);
    if (!infer_model_exp) {
        std::cerr << "Failed to create infer model, status = " << infer_model_exp.status() << std::endl;
        throw std::runtime_error("Failed to create infer model");
    }
    this->infer_model = infer_model_exp.release();

    this->input_buffer_guards.reserve(this->infer_model->inputs().size());
    this->output_buffer_guards.reserve(this->infer_model->outputs().size());
    for (auto& output_vstream_info : this->infer_model->hef().get_output_vstream_infos().release()) {
        std::string name(output_vstream_info.name);
        this->output_vstream_info_by_name[name] = output_vstream_info;
    }
    this->configured_infer_model = this->infer_model->configure().expect("Failed to create configured infer model");
    this->bindings = configured_infer_model.create_bindings().expect("Failed to create infer bindings");
}

const std::vector<hailort::InferModel::InferStream>& AsyncModelInfer::get_inputs(){
    return std::move(this->infer_model->inputs());
}

const std::vector<hailort::InferModel::InferStream>& AsyncModelInfer::get_outputs(){
    return std::move(this->infer_model->outputs());
}

const std::shared_ptr<hailort::InferModel> AsyncModelInfer::get_infer_model(){
    return this->infer_model;
}

void AsyncModelInfer::infer(
    std::shared_ptr<cv::Mat> input_data,
    std::function<void(const hailort::AsyncInferCompletionInfo&,
                    const std::vector<std::pair<uint8_t*, hailo_vstream_info_t>> &)> callback)
{
    set_input_buffers(input_data);
    auto output_data_and_infos = prepare_output_buffers();
    wait_and_run_async(output_data_and_infos, callback);
}

void AsyncModelInfer::set_input_buffers(const std::shared_ptr<cv::Mat> &input_data)
{
    for (const auto &input_name : infer_model->get_input_names()) {
        size_t frame_size = infer_model->input(input_name)->get_frame_size();
        auto status = bindings.input(input_name)->set_buffer(MemoryView(input_data->data, frame_size));
        if (HAILO_SUCCESS != status) {
            std::cerr << "Failed to set infer input buffer, status = " << status << std::endl;
        }
        input_buffer_guards.push_back(input_data);
    }
}

std::vector<std::pair<uint8_t*, hailo_vstream_info_t>> AsyncModelInfer::prepare_output_buffers()
{
    std::vector<std::pair<uint8_t*, hailo_vstream_info_t>> result;
    for (const auto &output_name : infer_model->get_output_names()) {
        size_t frame_size = infer_model->output(output_name)->get_frame_size();
        output_data_holder = page_aligned_alloc(frame_size);
        auto status = bindings.output(output_name)->set_buffer(MemoryView(output_data_holder.get(), frame_size));
        if (HAILO_SUCCESS != status) {
            std::cerr << "Failed to set infer output buffer, status = " << status << std::endl;
        }
        result.push_back(std::make_pair(
            bindings.output(output_name)->get_buffer()->data(),
            output_vstream_info_by_name[output_name]
        ));
        output_buffer_guards.push_back(output_data_holder);
    }
    return result;
}

void AsyncModelInfer::wait_and_run_async(
    const std::vector<std::pair<uint8_t*, hailo_vstream_info_t>> &output_data_and_infos,
    std::function<void(const hailort::AsyncInferCompletionInfo&,
                    const std::vector<std::pair<uint8_t*, hailo_vstream_info_t>> &)> callback)
{
    auto status = configured_infer_model.wait_for_async_ready(std::chrono::milliseconds(1000));
    if (HAILO_SUCCESS != status) {
        std::cerr << "Failed wait_for_async_ready, status = " << status << std::endl;
    }
    auto job = configured_infer_model.run_async(
        bindings,
        [callback, output_data_and_infos](const hailort::AsyncInferCompletionInfo& info)
        {
            // callback sent by the applicative side
            callback(info, output_data_and_infos);
        }
    );
    if (!job) {
        std::cerr << "Failed to start async infer job, status = " << job.status() << std::endl;
    }
    job->detach();
}