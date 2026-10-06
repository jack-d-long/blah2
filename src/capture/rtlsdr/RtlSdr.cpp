#include "RtlSdr.h"

#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <fftw3.h>

// constructor
RtlSdr::RtlSdr(std::string _type, uint32_t _fc, uint32_t _fs,
  std::string _path, bool *_saveIq, std::vector<std::string> _serial,
  std::vector<double> _gain, uint32_t _nCorr, uint32_t _maxLag,
  double _interval, double _minPeakRatio)
    : Source(_type, _fc, _fs, _path, _saveIq)
{
  if (_serial.size() != N_CHANNEL || _gain.size() != N_CHANNEL)
  {
    throw std::invalid_argument("[RtlSdr] Require 2 serials and 2 gains.");
  }
  if (_serial[0] == _serial[1])
  {
    throw std::invalid_argument("[RtlSdr] Serials must be unique.");
  }
  if (_maxLag >= _nCorr)
  {
    throw std::invalid_argument("[RtlSdr] Require maxLag < nCorr.");
  }
  serial = _serial;
  gain = _gain;
  nCorr = _nCorr;
  maxLag = _maxLag;
  interval = _interval;
  minPeakRatio = _minPeakRatio;
  running = false;
  resyncRequested = false;
  syncState = "acquiring";
  syncOffset = 0;
  syncPeakRatio = 0;
  syncTime = 0;
  nAcquire = 0;
  for (int i = 0; i < N_CHANNEL; i++)
  {
    dev[i] = nullptr;
    nReceived[i] = 0;
    context[i] = {this, i};
  }
}

void RtlSdr::start()
{
  int status;

  // wait until both devices are connected
  std::string missing = "";
  while (true)
  {
    std::string nowMissing = "";
    for (int i = 0; i < N_CHANNEL; i++)
    {
      if (rtlsdr_get_index_by_serial(serial[i].c_str()) < 0)
      {
        nowMissing += (nowMissing.empty() ? "" : ", ") + serial[i];
      }
    }
    if (nowMissing.empty())
    {
      break;
    }
    if (nowMissing != missing)
    {
      std::cout << "[RtlSdr] Waiting for device " << nowMissing << 
        "." << std::endl;
      set_status("waiting for " + nowMissing, 0, 0);
      missing = nowMissing;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  set_status("acquiring", 0, 0);

  for (int i = 0; i < N_CHANNEL; i++)
  {
    int index = rtlsdr_get_index_by_serial(serial[i].c_str());
    std::cout << "[RtlSdr] Setting up channel " << i << " (serial " <<
      serial[i] << ", index " << index << ")." << std::endl;

    status = rtlsdr_open(&dev[i], index);
    check_status(status, "Failed to open device.");
    status = rtlsdr_set_sample_rate(dev[i], fs);
    check_status(status, "Failed to set sample rate.");
    status = rtlsdr_set_dithering(dev[i], 0); // disable dither
    check_status(status, "Failed to disable dithering.");
    status = rtlsdr_set_center_freq(dev[i], fc);
    check_status(status, "Failed to set center frequency.");
    status = rtlsdr_set_tuner_gain_mode(dev[i], 1); // manual gain
    check_status(status, "Failed to set manual gain mode.");

    // snap gain to nearest valid tuner gain (tenths of dB)
    int nGains = rtlsdr_get_tuner_gains(dev[i], nullptr);
    check_status(nGains, "Failed to get number of gains.");
    std::vector<int> validGains(nGains);
    status = rtlsdr_get_tuner_gains(dev[i], validGains.data());
    check_status(status, "Failed to get gains.");
    int target = static_cast<int>(std::lround(gain[i] * 10));
    int nearest = validGains[0];
    for (int g : validGains)
    {
      if (std::abs(g - target) < std::abs(nearest - target))
      {
        nearest = g;
      }
    }
    status = rtlsdr_set_tuner_gain(dev[i], nearest);
    check_status(status, "Failed to set gain.");
    std::cout << "[RtlSdr] Channel " << i << " gain " << gain[i] <<
      " dB set to " << nearest / 10.0 << " dB." << std::endl;

    status = rtlsdr_set_agc_mode(dev[i], 0);
    check_status(status, "Failed to disable RTL AGC.");
  }

  // reset both buffers together just before streaming
  for (int i = 0; i < N_CHANNEL; i++)
  {
    status = rtlsdr_reset_buffer(dev[i]);
    check_status(status, "Failed to reset buffer.");
  }
}

void RtlSdr::stop()
{
  running = false;
  fifoCond.notify_all();
  for (int i = 0; i < N_CHANNEL; i++)
  {
    if (dev[i] != nullptr)
    {
      rtlsdr_cancel_async(dev[i]);
    }
  }
}

void RtlSdr::process(IqData *buffer1, IqData *buffer2)
{
  running = true;

  std::vector<std::thread> threads;
  for (int i = 0; i < N_CHANNEL; i++)
  {
    threads.emplace_back([this, i]{
      int status = rtlsdr_read_async(dev[i], callback, &context[i],
        0, BUFFER_LENGTH);
      if (running)
      {
        // device lost, exit so the container restarts capture
        std::cerr << "[RtlSdr] Async read ended on channel " << i <<
          " (status " << status << ")." << std::endl;
        exit(1);
      }
    });
  }

  align(buffer1, buffer2);

  for (auto& thread : threads)
  {
    thread.join();
  }
}

void RtlSdr::callback(unsigned char *buf, uint32_t len, void *ctx)
{
  CallbackContext *context = (CallbackContext*)ctx;
  RtlSdr *self = context->self;
  int channel = context->channel;

  {
    std::lock_guard<std::mutex> lock(self->fifoMutex);
    // discard startup transfers, which may not be contiguous
    if (self->nReceived[channel] >= self->fs / 2)
    {
      self->fifo[channel].insert(self->fifo[channel].end(), buf, buf + len);
    }
    self->nReceived[channel] += len / 2;
  }
  self->fifoCond.notify_one();
}

bool RtlSdr::acquire()
{
  const size_t nBytes = 2 * static_cast<size_t>(nCorr);
  const size_t maxBytes = 2 * static_cast<size_t>(fs) * 4;
  std::vector<std::complex<double>> x(nCorr), y(nCorr);

  // wait for enough samples on both channels
  {
    std::unique_lock<std::mutex> lock(fifoMutex);
    fifoCond.wait(lock, [&]{
      if (fifo[0].size() > maxBytes || fifo[1].size() > maxBytes)
      {
        std::cerr << "[RtlSdr] Channel stalled during acquire (FIFO " <<
          fifo[0].size() / 2 << ", " << fifo[1].size() / 2 <<
          " samples), flushing." << std::endl;
        fifo[0].clear();
        fifo[1].clear();
      }
      return !running ||
        (fifo[0].size() >= nBytes && fifo[1].size() >= nBytes);
    });
    if (!running)
    {
      return false;
    }
    for (size_t i = 0; i < nCorr; i++)
    {
      x[i] = {fifo[0][2*i] - 127.5, fifo[0][2*i+1] - 127.5};
      y[i] = {fifo[1][2*i] - 127.5, fifo[1][2*i+1] - 127.5};
    }
  }

  double peakRatio;
  double lag = estimate_lag(x, y, maxLag, peakRatio);

  std::lock_guard<std::mutex> lock(fifoMutex);
  if (peakRatio < minPeakRatio)
  {
    std::cout << "[RtlSdr] Acquire failed: offset " << lag <<
      " samples, peak ratio " << peakRatio << " < " << minPeakRatio <<
      ", retrying." << std::endl;
    set_status("acquiring", lag, peakRatio);
    fifo[0].erase(fifo[0].begin(), fifo[0].begin() + nBytes);
    fifo[1].erase(fifo[1].begin(), fifo[1].begin() + nBytes);
    return false;
  }

  // drop samples from the leading channel
  long lagInt = std::lround(lag);
  int lead = lagInt >= 0 ? 0 : 1;
  size_t nDrop = 2 * static_cast<size_t>(std::labs(lagInt));
  fifo[lead].erase(fifo[lead].begin(), fifo[lead].begin() + nDrop);

  std::cout << "[RtlSdr] Acquired: offset " << std::fixed <<
    std::setprecision(2) << lag << " samples (" <<
    1000.0 * lag / fs << " ms), peak ratio " << peakRatio <<
    ", dropped " << std::labs(lagInt) << " from " <<
    (lead == 0 ? "reference" : "surveillance") << "." <<
    std::defaultfloat << std::endl;
  {
    std::lock_guard<std::mutex> statusLock(statusMutex);
    nAcquire++;
  }
  set_status("aligned", lag - lagInt, peakRatio);
  return true;
}

void RtlSdr::align(IqData *buffer1, IqData *buffer2)
{
  const size_t maxBytes = 2 * static_cast<size_t>(fs) * 4;
  std::vector<uint8_t> raw[N_CHANNEL];
  std::vector<uint8_t> interleaved;
  std::vector<std::complex<double>> snapX, snapY;
  bool aligned = false;
  auto lastCheck = std::chrono::steady_clock::now();

  while (running)
  {
    if (resyncRequested.exchange(false))
    {
      std::cout << "[RtlSdr] Re-sync requested." << std::endl;
      set_status("acquiring", 0, 0);
      aligned = false;
    }
    if (!aligned)
    {
      aligned = acquire();
      lastCheck = std::chrono::steady_clock::now();
      snapX.clear();
      snapY.clear();
      continue;
    }

    // move equal sample counts out of both staging FIFOs
    size_t n;
    {
      std::unique_lock<std::mutex> lock(fifoMutex);
      fifoCond.wait_for(lock, std::chrono::milliseconds(100), [&]{
        return !running || (!fifo[0].empty() && !fifo[1].empty());
      });
      n = std::min(fifo[0].size(), fifo[1].size());
      if (std::max(fifo[0].size(), fifo[1].size()) - n > maxBytes)
      {
        std::cerr << "[RtlSdr] Channel stalled (FIFO " <<
          fifo[0].size() / 2 << ", " << fifo[1].size() / 2 <<
          " samples), re-acquiring." << std::endl;
        fifo[0].clear();
        fifo[1].clear();
        aligned = false;
        continue;
      }
      if (n > maxBytes)
      {
        // aligner behind, drop the same count from both
        size_t nDrop = n - maxBytes;
        std::cerr << "[RtlSdr] Staging overflow, dropping " <<
          nDrop / 2 << " samples from both channels." << std::endl;
        fifo[0].erase(fifo[0].begin(), fifo[0].begin() + nDrop);
        fifo[1].erase(fifo[1].begin(), fifo[1].begin() + nDrop);
        n = maxBytes;
      }
      for (int i = 0; i < N_CHANNEL; i++)
      {
        raw[i].assign(fifo[i].begin(), fifo[i].begin() + n);
        fifo[i].erase(fifo[i].begin(), fifo[i].begin() + n);
      }
    }
    if (n == 0)
    {
      continue;
    }

    // save aligned samples as [I1 Q1 I2 Q2]
    if (*saveIq && saveIqFile.is_open())
    {
      interleaved.resize(2 * n);
      for (size_t i = 0; i < n; i += 2)
      {
        interleaved[2*i] = raw[0][i];
        interleaved[2*i+1] = raw[0][i+1];
        interleaved[2*i+2] = raw[1][i];
        interleaved[2*i+3] = raw[1][i+1];
      }
      saveIqFile.write(reinterpret_cast<char*>(interleaved.data()),
        interleaved.size());
    }

    buffer1->lock();
    buffer2->lock();
    for (size_t i = 0; i < n; i += 2)
    {
      buffer1->push_back({raw[0][i] - 127.5, raw[0][i+1] - 127.5});
      buffer2->push_back({raw[1][i] - 127.5, raw[1][i+1] - 127.5});
    }
    buffer1->unlock();
    buffer2->unlock();

    // monitor alignment on a snapshot of aligned samples
    std::chrono::duration<double> elapsed =
      std::chrono::steady_clock::now() - lastCheck;
    if (elapsed.count() < interval)
    {
      continue;
    }
    for (size_t i = 0; i < n && snapX.size() < nCorr; i += 2)
    {
      snapX.push_back({raw[0][i] - 127.5, raw[0][i+1] - 127.5});
      snapY.push_back({raw[1][i] - 127.5, raw[1][i+1] - 127.5});
    }
    if (snapX.size() < nCorr)
    {
      continue;
    }
    double peakRatio;
    double lag = estimate_lag(snapX, snapY, maxLag, peakRatio);
    uint64_t received[N_CHANNEL];
    {
      std::lock_guard<std::mutex> lock(fifoMutex);
      received[0] = nReceived[0];
      received[1] = nReceived[1];
    }
    std::cout << "[RtlSdr] Sync check: offset " << std::fixed <<
      std::setprecision(2) << lag << " samples, peak ratio " <<
      peakRatio << std::defaultfloat << ", received " << received[0] <<
      " / " << received[1] << "." << std::endl;
    if (peakRatio >= minPeakRatio && std::abs(lag) > 0.75)
    {
      std::cout << "[RtlSdr] Alignment lost, re-acquiring." << std::endl;
      aligned = false;
    }
    set_status(aligned ? "aligned" : "acquiring", lag, peakRatio);
    lastCheck = std::chrono::steady_clock::now();
    snapX.clear();
    snapY.clear();
  }
}

double RtlSdr::estimate_lag(const std::vector<std::complex<double>> &x,
  const std::vector<std::complex<double>> &y, uint32_t maxLag,
  double &peakRatio)
{
  size_t n = std::min(x.size(), y.size());
  size_t nfft = 1;
  while (nfft < 2 * n)
  {
    nfft <<= 1;
  }
  std::vector<std::complex<double>> a(nfft), b(nfft);

  fftw_plan fftA = fftw_plan_dft_1d(nfft,
    reinterpret_cast<fftw_complex *>(a.data()),
    reinterpret_cast<fftw_complex *>(a.data()), FFTW_FORWARD, FFTW_ESTIMATE);
  fftw_plan fftB = fftw_plan_dft_1d(nfft,
    reinterpret_cast<fftw_complex *>(b.data()),
    reinterpret_cast<fftw_complex *>(b.data()), FFTW_FORWARD, FFTW_ESTIMATE);
  fftw_plan ifftA = fftw_plan_dft_1d(nfft,
    reinterpret_cast<fftw_complex *>(a.data()),
    reinterpret_cast<fftw_complex *>(a.data()), FFTW_BACKWARD, FFTW_ESTIMATE);

  // remove DC, which would otherwise correlate at lag 0
  std::complex<double> meanX = 0, meanY = 0;
  for (size_t i = 0; i < n; i++)
  {
    meanX += x[i];
    meanY += y[i];
  }
  meanX /= static_cast<double>(n);
  meanY /= static_cast<double>(n);
  std::fill(a.begin(), a.end(), 0);
  std::fill(b.begin(), b.end(), 0);
  for (size_t i = 0; i < n; i++)
  {
    a[i] = x[i] - meanX;
    b[i] = y[i] - meanY;
  }

  // c[k] = sum x[i+k] conj(y[i])
  fftw_execute(fftA);
  fftw_execute(fftB);
  for (size_t i = 0; i < nfft; i++)
  {
    a[i] *= std::conj(b[i]);
  }
  fftw_execute(ifftA);

  fftw_destroy_plan(fftA);
  fftw_destroy_plan(fftB);
  fftw_destroy_plan(ifftA);

  long maxLagL = std::min<long>(maxLag, static_cast<long>(n) - 1);
  long nfftL = static_cast<long>(nfft);
  auto mag = [&](long k){ return std::abs(a[k < 0 ? nfftL + k : k]); };
  std::vector<double> mags;
  mags.reserve(2 * maxLagL + 1);
  long peakLag = 0;
  double peak = -1;
  for (long k = -maxLagL; k <= maxLagL; k++)
  {
    double m = mag(k);
    mags.push_back(m);
    if (m > peak)
    {
      peak = m;
      peakLag = k;
    }
  }
  std::nth_element(mags.begin(), mags.begin() + mags.size() / 2, mags.end());
  double median = mags[mags.size() / 2];
  peakRatio = median > 0 ? peak / median : 0;

  // parabolic interpolation around the peak
  double lag = static_cast<double>(peakLag);
  if (peakLag > -maxLagL && peakLag < maxLagL)
  {
    double m0 = mag(peakLag - 1);
    double m1 = peak;
    double m2 = mag(peakLag + 1);
    double denom = m0 - 2 * m1 + m2;
    if (denom != 0)
    {
      lag += 0.5 * (m0 - m2) / denom;
    }
  }
  return lag;
}

void RtlSdr::replay(IqData *buffer1, IqData *buffer2, std::string _file, bool _loop)
{
  FILE *file = fopen(_file.c_str(), "rb");
  if (file == nullptr)
  {
    std::cerr << "[RtlSdr] Can not open replay file: " << _file << std::endl;
    exit(1);
  }
  std::cout << "[RtlSdr] Replaying " << _file << "." << std::endl;

  std::vector<uint8_t> block(4 * 65536);
  while (true)
  {
    size_t nPair = fread(block.data(), 1, block.size(), file) / 4;
    if (nPair == 0)
    {
      if (!_loop)
      {
        break;
      }
      rewind(file);
      continue;
    }
    size_t i = 0;
    while (i < nPair)
    {
      buffer1->lock();
      buffer2->lock();
      while (i < nPair && buffer1->get_length() < buffer1->get_n())
      {
        buffer1->push_back({block[4*i] - 127.5, block[4*i+1] - 127.5});
        buffer2->push_back({block[4*i+2] - 127.5, block[4*i+3] - 127.5});
        i++;
      }
      buffer1->unlock();
      buffer2->unlock();
      if (i < nPair)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  }
  fclose(file);
}

void RtlSdr::set_status(std::string state, double offset, double peakRatio)
{
  std::lock_guard<std::mutex> lock(statusMutex);
  syncState = state;
  syncOffset = offset;
  syncPeakRatio = peakRatio;
  syncTime = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

void RtlSdr::resync()
{
  resyncRequested = true;
}

std::string RtlSdr::status_json()
{
  uint64_t received[N_CHANNEL];
  {
    std::lock_guard<std::mutex> lock(fifoMutex);
    received[0] = nReceived[0];
    received[1] = nReceived[1];
  }
  std::lock_guard<std::mutex> lock(statusMutex);
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2) << "{\"device\":\"RtlSdr\"," <<
    "\"state\":\"" << syncState << "\"," <<
    "\"offset\":" << syncOffset << "," <<
    "\"peakRatio\":" << syncPeakRatio << "," <<
    "\"time\":" << syncTime << "," <<
    "\"nAcquire\":" << nAcquire << "," <<
    "\"received\":[" << received[0] << "," << received[1] << "]}";
  return oss.str();
}

void RtlSdr::check_status(int status, std::string message)
{
  if (status < 0)
  {
    throw std::runtime_error("[RtlSdr] " + message);
  }
}
