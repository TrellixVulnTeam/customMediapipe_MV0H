#include "ump_pipeline.h"
#include "ump_observer.h"

#include "mediapipe/framework/formats/image_frame.h"
#include "mediapipe/framework/formats/image_frame_opencv.h"

#include "mediapipe/framework/port/opencv_imgproc_inc.h"
#include "mediapipe/framework/port/opencv_video_inc.h"
#include "mediapipe/framework/port/opencv_highgui_inc.h"

#include "mediapipe/framework/port/parse_text_proto.h"
#include "mediapipe/framework/port/file_helpers.h"
#include "mediapipe/util/resource_util.h"

#include <chrono>
#include <thread>

UmpPipeline::UmpPipeline()
{
	log_d("+UmpPipeline");
}

UmpPipeline::~UmpPipeline()
{
	log_d("~UmpPipeline");
	Stop();
}

//��mutator
void UmpPipeline::SetGraphConfiguration(const char* filename)
{
	log_i(strf("SetGraphConfiguration: %s", filename));
	config_filename = filename;
}
void UmpPipeline::SetCaptureFromFile(const char* filename)
{
	log_i(strf("SetCaptureFromFile: %s", filename));
	input_filename = filename;
}
void UmpPipeline::SetCaptureParams(int in_cam_id, int in_cam_api, int in_cam_resx, int in_cam_resy, int in_cam_fps)
{
	log_i(strf("SetCaptureParams: cam=%d api=%d w=%d h=%d fps=%d", in_cam_id, in_cam_api, in_cam_resx, in_cam_resy, in_cam_fps));
	cam_id = in_cam_id;
	cam_api = in_cam_api;
	cam_resx = in_cam_resx;
	cam_resy = in_cam_resy;
	cam_fps = in_cam_fps;
}
void UmpPipeline::SetOverlay(bool in_overlay)
{
	log_i(strf("SetOverlay: %d", (in_overlay ? 1 : 0)));
	overlay = in_overlay;
}

IUmpObserver* UmpPipeline::CreateObserver(const char* stream_name)
{
	log_i(strf("CreateObserver: %s", stream_name));
	if (run_flag)
	{
		log_e("Invalid state: pipeline running");
		return nullptr;
	}
	auto* observer = new UmpObserver(stream_name);
	observer->AddRef();
	observers.emplace_back(observer);
	return observer;
}

bool UmpPipeline::Start()
{
	Stop();
	try
	{
		log_i("UmpPipeline::Start");
		frame_id = 0;
		frame_ts = 0;
		run_flag = true;
		worker = std::make_unique<std::thread>([this]() { this->WorkerThread(); });
		log_i("UmpPipeline::Start OK");
		return true;
	}
	catch (const std::exception& ex)
	{
		log_e(ex.what());
	}
	return false;
}

void UmpPipeline::Stop()
{
	try
	{
		run_flag = false;
		if (worker)
		{
			log_i("UmpPipeline::Stop");
			worker->join();
			worker.reset();
			log_i("UmpPipeline::Stop OK");
		}
	}
	catch (const std::exception& ex)
	{
		log_e(ex.what());
	}
}

void UmpPipeline::WorkerThread()
{
	//worker thread mainly do 2 things: RunImpl and ShutdownImpl

	log_i("Enter WorkerThread");
	// RUN
	try
	{
		auto status = this->RunImpl();
		if (!status.ok())
		{
			std::string msg(status.message());
			log_e(msg);
		}
	}
	catch (const std::exception& ex)
	{
		log_e(ex.what());
	}
	// SHUTDOWN
	try
	{
		ShutdownImpl();
	}
	catch (const std::exception& ex)
	{
		log_e(ex.what());
	}
	log_i("Leave WorkerThread");
}

void UmpPipeline::ShutdownImpl()
{
	log_i("UmpPipeline::Shutdown");

	graph.reset();
	observers.clear();

	if (overlay)
		cv::destroyAllWindows();

	log_i("UmpPipeline::Shutdown OK");
}

inline double get_timestamp_us() // microseconds
{
	return (double)cv::getTickCount() / (double)cv::getTickFrequency() * 1e6;
}

absl::Status UmpPipeline::RunImpl()
{
	constexpr char kInputStream[] = "input_video";
	constexpr char kOutputStream[] = "output_video";
	constexpr char kWindowName[] = "MediaPipe";

	log_i("UmpPipeline::Run");

	// init mediapipe

	//create graph
	std::string config_str;
	RET_CHECK_OK(LoadGraphConfig(config_filename, config_str));//get pointer of config (passed to-config_str)
	log_i("ParseTextProto");

	mediapipe::CalculatorGraphConfig config;
	RET_CHECK(mediapipe::ParseTextProto<mediapipe::CalculatorGraphConfig>(config_str, &config));

	log_i("CalculatorGraph::Initialize");
	graph.reset(new mediapipe::CalculatorGraph());
	RET_CHECK_OK(graph->Initialize(config));
	//point the stream pointers to location in the graph

	for (auto& iter : observers)
	{
		RET_CHECK_OK(iter->ObserveOutputStream(graph.get()));
	}

	std::unique_ptr<mediapipe::OutputStreamPoller> output_poller;
	if (overlay)
	{
		//ASSIGN_OR_RETURN(mediapipe::OutputStreamPoller poller, graph->AddOutputStreamPoller(kOutputStream));
		auto output_poller_sop = graph->AddOutputStreamPoller(kOutputStream);
		RET_CHECK(output_poller_sop.ok());
		output_poller = std::make_unique<mediapipe::OutputStreamPoller>(std::move(output_poller_sop.value()));
	}

	// init opencv

	log_i("VideoCapture::open");
	cv::VideoCapture capture;
	use_camera = input_filename.empty();

	if (use_camera)
	{
		#if defined(_WIN32)
		if (cam_api == cv::CAP_ANY)
		{
			// CAP_MSMF is broken on windows! use CAP_DSHOW by default, also see: https://github.com/opencv/opencv/issues/17687
			cam_api = cv::CAP_DSHOW;
		}
		#endif

		capture.open(cam_id, cam_api);
	}
	else
	{
		capture.open(*input_filename);
	}

	RET_CHECK(capture.isOpened());

	if (use_camera)
	{
		if (cam_resx > 0 && cam_resy > 0)
		{
			capture.set(cv::CAP_PROP_FRAME_WIDTH, cam_resx);
			capture.set(cv::CAP_PROP_FRAME_HEIGHT, cam_resy);
		}

		if (cam_fps > 0)
			capture.set(cv::CAP_PROP_FPS, cam_fps);
	}

	const int cap_resx = (int)capture.get(cv::CAP_PROP_FRAME_WIDTH);
	const int cap_resy = (int)capture.get(cv::CAP_PROP_FRAME_HEIGHT);
	const double cap_fps = (double)capture.get(cv::CAP_PROP_FPS);
	log_i(strf("CAPS: w=%d h=%d fps=%f", cap_resx, cap_resy, cap_fps));

	if (overlay)
		cv::namedWindow(kWindowName, cv::WINDOW_AUTOSIZE);

	// start

	cv::Mat camera_frame_raw;
	cv::Mat camera_frame;

	log_i("CalculatorGraph::StartRun");
	RET_CHECK_OK(graph->StartRun({}));

	double t0 = get_timestamp_us();

	log_i("MAIN LOOP");
	while (run_flag)
	{
		double t1 = get_timestamp_us();
		double dt = t1 - t0;
		t0 = t1;

		PROF_NAMED("pipeline_tick");

		{
			PROF_NAMED("capture_frame");
			capture >> camera_frame_raw;
		}

		if (!use_camera && camera_frame_raw.empty())
		{
			log_i("VideoCapture: EOF");
			break;
		}

		const double frame_timestamp_us = get_timestamp_us();
		frame_ts = frame_timestamp_us;

		{
			PROF_NAMED("process_frame");

			cv::cvtColor(camera_frame_raw, camera_frame, cv::COLOR_BGR2RGB);
			if (use_camera)
				cv::flip(camera_frame, camera_frame, 1);

			auto input_frame = absl::make_unique<mediapipe::ImageFrame>(
				mediapipe::ImageFormat::SRGB, camera_frame.cols, camera_frame.rows,
				mediapipe::ImageFrame::kDefaultAlignmentBoundary);

			cv::Mat input_frame_mat = mediapipe::formats::MatView(input_frame.get());
			camera_frame.copyTo(input_frame_mat);

			RET_CHECK_OK(graph->AddPacketToInputStream(
				kInputStream,
				mediapipe::Adopt(input_frame.release())
				.At(mediapipe::Timestamp((size_t)frame_timestamp_us))));
		}

		// draw overlay
		if (overlay && output_poller)
		{
			PROF_NAMED("draw_overlay");

			mediapipe::Packet packet;
			if (!output_poller->Next(&packet))
			{
				log_e("OutputStreamPoller::Next failed");
				break;
			}

			auto& output_frame = packet.Get<mediapipe::ImageFrame>();
			cv::Mat output_frame_mat = mediapipe::formats::MatView(&output_frame);

			auto stat = strf("%.0f | %.4f | %" PRIu64 "", frame_ts, dt * 0.001, frame_id);
			cv::putText(output_frame_mat, *stat, cv::Point(10, 20), cv::FONT_HERSHEY_PLAIN, 1, cv::Scalar(0, 255, 0));

			cv::cvtColor(output_frame_mat, output_frame_mat, cv::COLOR_RGB2BGR);
			//FIXME
			//cv::imshow(kWindowName, output_frame_mat);
			//cv::waitKey(1); // required for cv::imshow
		}

		// wait for next frame (when playing from file)
		if (!use_camera && cap_fps > 0.0)
		{
			PROF_NAMED("wait_next_frame");

			const double frame_us = (1.0 / cap_fps) * 1e6;
			for (;;)
			{
				const double cur_timestamp_us = get_timestamp_us();
				const double delta = fabs(cur_timestamp_us - frame_timestamp_us);
				if (delta >= frame_us)
					break;
				else
					std::this_thread::sleep_for(std::chrono::microseconds((size_t)(frame_us - delta)));
			}
		}

		frame_id++;
	}

	log_i("CalculatorGraph::CloseInputStream");
	graph->CloseInputStream(kInputStream);
	graph->WaitUntilDone();

	return absl::OkStatus();
}

// allows multiple files separated by ';'
absl::Status UmpPipeline::LoadGraphConfig(const std::string& filename, std::string& out_str)
{
	log_i(strf("LoadGraphConfig: %s", filename.c_str()));

	out_str.clear();
	out_str.reserve(4096);

	std::string sub_str;
	sub_str.reserve(1024);

	std::stringstream filename_ss(filename);
	std::string sub_name;

	while(std::getline(filename_ss, sub_name, ';'))
	{
		sub_str.clear();
		RET_CHECK_OK(LoadResourceFile(sub_name, sub_str));
		out_str.append(sub_str);
	}

	return absl::OkStatus();
}

absl::Status UmpPipeline::LoadResourceFile(const std::string& filename, std::string& out_str)
{
	log_i(strf("LoadResourceFile: %s", filename.c_str()));

	out_str.clear();

	std::string path;
	ASSIGN_OR_RETURN(path, mediapipe::PathToResourceAsFile(filename));

	RET_CHECK_OK(mediapipe::file::GetContents(path, &out_str));

	return absl::OkStatus();
}

void UmpPipeline::LogProfilerStats() {
	#if defined(PROF_ENABLE)
		log_i(std::string(PROF_SUMMARY));
	#endif
}
