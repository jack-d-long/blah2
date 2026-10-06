#include "Capture.h"
#include "rspduo/RspDuo.h"
#include "usrp/Usrp.h"
#include "hackrf/HackRf.h"
#include "kraken/Kraken.h"
#include "rtlsdr/RtlSdr.h"
#include <iostream>
#include <thread>
#include <httplib.h>
#include "rapidjson/document.h"

// constants
const std::string Capture::VALID_TYPE[5] = {"RspDuo", "Usrp", "HackRF", "Kraken", "RtlSdr"};

// constructor
Capture::Capture(std::string _type, uint32_t _fs, uint32_t _fc, std::string _path)
{
  type = _type;
  fs = _fs;
  fc = _fc;
  path = _path;
  replay = false;
  saveIq = false;
  paused = false;
}

void Capture::process(IqData *buffer1, IqData *buffer2, c4::yml::NodeRef config, 
  std::string ip_capture, uint16_t port_capture)
{
  std::cout << "Setting up device " + type << std::endl;

  device = factory_source(type, config);

  // capture status thread
  std::thread t1([&]{
    while (true)
    {
      httplib::Client cli("http://" + ip_capture + ":" 
        + std::to_string(port_capture));
      httplib::Result res = cli.Get("/capture");

      // if capture status changed
      if (res && (res->body == "true") != saveIq)
      {
        saveIq = res->body == "true";
        if (saveIq)
        {
          device->open_file();
        }
        else
        {
          device->close_file();
        }
      }

      // processing control
      res = cli.Get("/control/state");
      if (res && res->status == 200)
      {
        rapidjson::Document document;
        document.Parse(res->body.c_str());
        if (!document.HasParseError() && document.IsObject() &&
          document.HasMember("paused") && document["paused"].IsBool() &&
          document.HasMember("version") && document["version"].IsString())
        {
          if (document["paused"].GetBool() != paused)
          {
            paused = document["paused"].GetBool();
            std::cout << "Processing " << (paused ? "paused" : "resumed") 
              << "." << std::endl;
          }
          std::string version = document["version"].GetString();
          if (controlVersion.empty())
          {
            controlVersion = version;
          }
          else if (version != controlVersion)
          {
            // restart (via container restart policy) to apply new config
            std::cout << "Config override changed, restarting." << std::endl;
            device->kill();
          }
        }
      }

      // device status
      std::string status = device->status_json();
      if (!status.empty())
      {
        cli.Post("/control/device", status, "application/json");
      }
      sleep(1);
    }
  });

  if (!replay)
  {
    device->start();
    device->process(buffer1, buffer2);
  }
  else
  {
    device->replay(buffer1, buffer2, file, loop);
  }
  t1.join();
}

std::unique_ptr<Source> Capture::factory_source(const std::string& type, c4::yml::NodeRef config)
{
    // SDRplay RSPduo
    if (type == VALID_TYPE[0])
    {
        int agcSetPoint, bandwidthNumber, gainReductionA, gainReductionB, lnaState;
        bool dabNotch, rfNotch;
        config["agcSetPoint"] >> agcSetPoint;
        config["bandwidthNumber"] >> bandwidthNumber;
        config["gainReduction"][0] >> gainReductionA;
        config["gainReduction"][1] >> gainReductionB;
        config["lnaState"] >> lnaState;
        config["dabNotch"] >> dabNotch;
        config["rfNotch"] >> rfNotch;
        return std::make_unique<RspDuo>(type, fc, fs, path, &saveIq,
          agcSetPoint, bandwidthNumber, gainReductionA, gainReductionB, lnaState,
          dabNotch, rfNotch);
    }
    // Usrp
    else if (type == VALID_TYPE[1])
    {
        std::string address, subdev;
        std::vector<std::string> antenna;
        std::vector<double> gain;
        std::string _antenna;
        double _gain;
        config["address"] >> address;
        config["subdev"] >> subdev;
        config["antenna"][0] >> _antenna;
        antenna.push_back(_antenna);
        config["antenna"][1] >> _antenna;
        antenna.push_back(_antenna);
        config["gain"][0] >> _gain;
        gain.push_back(_gain);
        config["gain"][1] >> _gain;
        gain.push_back(_gain);
        return std::make_unique<Usrp>(type, fc, fs, path, &saveIq, 
          address, subdev, antenna, gain);
    }
    // HackRF
    else if (type == VALID_TYPE[2])
    {
      std::vector<std::string> serial;
      std::vector<uint32_t> gainLna, gainVga;
      std::vector<bool> ampEnable;
      std::string _serial;
      uint32_t gain;
      int _gain;
      bool _ampEnable;
      config["serial"][0] >> _serial;
      serial.push_back(_serial);
      config["serial"][1] >> _serial;
      serial.push_back(_serial);
      config["gain_lna"][0] >> _gain;
      gain = static_cast<uint32_t> (_gain);
      gainLna.push_back(gain);
      config["gain_lna"][1] >> _gain;
      gain = static_cast<uint32_t>(_gain);
      gainLna.push_back(gain);
      config["gain_vga"][0] >> _gain;
      gain = static_cast<uint32_t>(_gain);
      gainVga.push_back(gain);
      config["gain_vga"][1] >> _gain;
      gain = static_cast<uint32_t>(_gain);
      gainVga.push_back(gain);
      config["amp_enable"][0] >> _ampEnable;
      ampEnable.push_back(_ampEnable);
      config["amp_enable"][1] >> _ampEnable;
      ampEnable.push_back(_ampEnable);
      return std::make_unique<HackRf>(type, fc, fs, path, &saveIq,
        serial, gainLna, gainVga, ampEnable);
    }
    // Kraken
    else if (type == VALID_TYPE[3])
    {
      std::vector<double> gain;
      float _gain;
      for (auto child : config["gain"].children())
      {
        c4::atof(child.val(), &_gain);
        gain.push_back(static_cast<double>(_gain));
      }
      return std::make_unique<Kraken>(type, fc, fs, path, &saveIq, gain);
    }
    // 2x clock-synchronised RTL-SDR
    else if (type == VALID_TYPE[4])
    {
      std::vector<std::string> serial;
      std::vector<double> gain;
      std::string _serial;
      double _gain;
      uint32_t nCorr = 262144, maxLag = 100000;
      double interval = 10, minPeakRatio = 10;
      config["serial"][0] >> _serial;
      serial.push_back(_serial);
      config["serial"][1] >> _serial;
      serial.push_back(_serial);
      config["gain"][0] >> _gain;
      gain.push_back(_gain);
      config["gain"][1] >> _gain;
      gain.push_back(_gain);
      if (config.has_child("sync"))
      {
        c4::yml::NodeRef sync = config["sync"];
        if (sync.has_child("nCorr")) sync["nCorr"] >> nCorr;
        if (sync.has_child("maxLag")) sync["maxLag"] >> maxLag;
        if (sync.has_child("interval")) sync["interval"] >> interval;
        if (sync.has_child("minPeakRatio")) sync["minPeakRatio"] >> minPeakRatio;
      }
      return std::make_unique<RtlSdr>(type, fc, fs, path, &saveIq,
        serial, gain, nCorr, maxLag, interval, minPeakRatio);
    }
    // handle unknown type
    std::cerr << "Error: Source type does not exist." << std::endl;
    return nullptr;
}

void Capture::set_replay(bool _loop, std::string _file)
{
  replay = true;
  loop = _loop;
  file = _file;
}
