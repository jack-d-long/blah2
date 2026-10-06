/// @file RtlSdr.h
/// @class RtlSdr
/// @brief A class to capture data on 2 clock-synchronised RTL-SDRs.
/// @details Devices are opened by serial number (reference, surveillance).
/// Sharing a clock removes frequency drift, but each dongle starts streaming
/// at a random instant and may silently drop samples over USB. Samples from
/// each device are written to private staging FIFOs. An aligner thread
/// cross-correlates the 2 channels, drops samples from the leading channel,
/// then moves samples to the blah2 buffers in equal-length pairs. The
/// alignment is re-checked periodically and re-acquired if lost.
/// Requires a librtlsdr which includes rtlsdr_set_dithering(),
/// such as krakenrf/librtlsdr.
/// Saved IQ files are interleaved uint8 [I1 Q1 I2 Q2] after alignment.
/// @author jack-d-long

#ifndef RTLSDR_H
#define RTLSDR_H

#include "capture/Source.h"
#include "data/IqData.h"

#include <stdint.h>
#include <string>
#include <vector>
#include <deque>
#include <complex>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <rtl-sdr.h>

class RtlSdr : public Source
{
private:

  /// @brief Number of channels (reference, surveillance).
  static const int N_CHANNEL = 2;

  /// @brief Bytes requested per async USB transfer.
  static const uint32_t BUFFER_LENGTH = 16 * 16384;

  /// @brief RTL-SDR devices.
  rtlsdr_dev_t *dev[N_CHANNEL];

  /// @brief Serial numbers [reference, surveillance].
  std::vector<std::string> serial;

  /// @brief Requested gain for each channel (dB).
  std::vector<double> gain;

  /// @brief Samples per channel used for each alignment correlation.
  uint32_t nCorr;

  /// @brief Largest start offset searched (samples).
  uint32_t maxLag;

  /// @brief Seconds between alignment re-checks.
  double interval;

  /// @brief Correlation peak / median required to trust an estimate.
  double minPeakRatio;

  /// @brief Staging FIFO of raw interleaved uint8 IQ per channel.
  std::deque<uint8_t> fifo[N_CHANNEL];

  /// @brief Total samples received per channel.
  uint64_t nReceived[N_CHANNEL];

  /// @brief Mutex for staging FIFOs.
  std::mutex fifoMutex;

  /// @brief Signalled when new samples arrive in a staging FIFO.
  std::condition_variable fifoCond;

  /// @brief True while capture is running.
  std::atomic<bool> running;

  /// @brief Mutex for sync status.
  std::mutex statusMutex;

  /// @brief Sync state ("acquiring" or "aligned").
  std::string syncState;

  /// @brief Last measured offset (samples).
  double syncOffset;

  /// @brief Last correlation peak / median.
  double syncPeakRatio;

  /// @brief Time of last acquire or sync check (POSIX ms).
  uint64_t syncTime;

  /// @brief Number of successful acquires.
  uint32_t nAcquire;

  /// @brief Update sync status.
  /// @param state Sync state.
  /// @param offset Measured offset (samples).
  /// @param peakRatio Correlation peak / median.
  /// @return Void.
  void set_status(std::string state, double offset, double peakRatio);

  /// @brief Context passed to each async callback.
  struct CallbackContext
  {
    RtlSdr *self;
    int channel;
  };

  /// @brief Callback contexts for each channel.
  CallbackContext context[N_CHANNEL];

  /// @brief Check status of API returns.
  /// @param status Return code of API call.
  /// @param message Message if API call error.
  /// @return Void.
  void check_status(int status, std::string message);

  /// @brief Callback function when buffer is filled.
  /// @param buf Pointer to buffer of IQ data.
  /// @param len Length of buffer.
  /// @param ctx Context data for callback.
  /// @return Void.
  static void callback(unsigned char *buf, uint32_t len, void *ctx);

  /// @brief Align staging FIFOs and stream sample pairs to blah2 buffers.
  /// @param buffer1 Buffer for reference samples.
  /// @param buffer2 Buffer for surveillance samples.
  /// @return Void.
  void align(IqData *buffer1, IqData *buffer2);

  /// @brief Estimate offset between channels from staging FIFOs.
  /// @details Drops samples from the leading channel on success.
  /// @return True if a trusted estimate was applied.
  bool acquire();

public:

  /// @brief Constructor.
  /// @param type The capture device type.
  /// @param fc Center frequency (Hz).
  /// @param fs Sampling frequency (Hz).
  /// @param path Path to save IQ data.
  /// @param saveIq True if IQ data to be saved.
  /// @param serial Serial numbers [reference, surveillance].
  /// @param gain Gain for each channel (dB).
  /// @param nCorr Samples per channel for each alignment correlation.
  /// @param maxLag Largest start offset searched (samples).
  /// @param interval Seconds between alignment re-checks.
  /// @param minPeakRatio Correlation peak / median to trust an estimate.
  /// @return The object.
  RtlSdr(std::string type, uint32_t fc, uint32_t fs, std::string path,
    bool *saveIq, std::vector<std::string> serial, std::vector<double> gain,
    uint32_t nCorr, uint32_t maxLag, double interval, double minPeakRatio);

  /// @brief Implement capture function on RTL-SDRs.
  /// @param buffer1 Buffer for reference samples.
  /// @param buffer2 Buffer for surveillance samples.
  /// @return Void.
  void process(IqData *buffer1, IqData *buffer2);

  /// @brief Call methods to start capture.
  /// @return Void.
  void start();

  /// @brief Call methods to gracefully stop capture.
  /// @return Void.
  void stop();

  /// @brief Implement replay function on RTL-SDRs.
  /// @param buffer1 Buffer for reference samples.
  /// @param buffer2 Buffer for surveillance samples.
  /// @param file Path to file to replay data from.
  /// @param loop True if samples should loop at EOF.
  /// @return Void.
  void replay(IqData *buffer1, IqData *buffer2, std::string file, bool loop);

  /// @brief Sync status for the control API.
  /// @return JSON string.
  std::string status_json();

  /// @brief Estimate lag of x relative to y by FFT cross-correlation.
  /// @details A positive lag means x leads, i.e. x[n + lag] matches y[n],
  /// so lag samples should be dropped from x.
  /// @param x First channel samples.
  /// @param y Second channel samples (same length as x).
  /// @param maxLag Largest absolute lag searched (samples, < length).
  /// @param peakRatio Output correlation peak / median magnitude.
  /// @return Lag estimate with parabolic sub-sample interpolation.
  static double estimate_lag(const std::vector<std::complex<double>> &x,
    const std::vector<std::complex<double>> &y, uint32_t maxLag,
    double &peakRatio);

};

#endif
