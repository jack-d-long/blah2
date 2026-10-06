/// @file TestRtlSdrSync.cpp
/// @brief Unit test for RtlSdr channel alignment.
/// @author jack-d-long

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "capture/rtlsdr/RtlSdr.h"

#include <random>
#include <cmath>

namespace
{
  /// @brief Random complex signal, uint8-like scale with DC offset.
  std::vector<std::complex<double>> make_signal(size_t n, unsigned seed)
  {
    std::mt19937 gen(seed);
    std::normal_distribution<double> dist(0.0, 20.0);
    std::vector<std::complex<double>> s(n);
    for (auto &v : s)
    {
      v = {dist(gen) + 3.0, dist(gen) - 2.0};
    }
    return s;
  }

  /// @brief Add independent noise.
  void add_noise(std::vector<std::complex<double>> &s, double sigma, unsigned seed)
  {
    std::mt19937 gen(seed);
    std::normal_distribution<double> dist(0.0, sigma);
    for (auto &v : s)
    {
      v += std::complex<double>(dist(gen), dist(gen));
    }
  }
}

/// @brief Lag recovered for both leading channels, with noise and phase.
TEST_CASE("Estimate_Lag", "[rtlsdr]")
{
  const size_t n = 65536;
  const uint32_t maxLag = 20000;
  long d = GENERATE(0L, 1L, -1L, 1234L, -1234L, 19000L, -19000L);

  auto s = make_signal(n + std::labs(d), 1);
  std::vector<std::complex<double>> x(n), y(n);
  std::complex<double> phase = std::polar(1.0, 1.1);
  for (size_t i = 0; i < n; i++)
  {
    // x[i + d] matches y[i]
    x[i] = d >= 0 ? s[i] : s[i - d];
    y[i] = (d >= 0 ? s[i + d] : s[i]) * phase;
  }
  add_noise(x, 20.0, 2);
  add_noise(y, 20.0, 3);

  double peakRatio;
  double lag = RtlSdr::estimate_lag(x, y, maxLag, peakRatio);
  CHECK(std::lround(lag) == d);
  CHECK(std::abs(lag - d) < 0.5);
  CHECK(peakRatio > 10);
}

/// @brief Uncorrelated channels are not trusted.
TEST_CASE("Estimate_Lag_Uncorrelated", "[rtlsdr]")
{
  auto x = make_signal(65536, 4);
  auto y = make_signal(65536, 5);
  double peakRatio;
  RtlSdr::estimate_lag(x, y, 20000, peakRatio);
  CHECK(peakRatio < 10);
}
