#include <array>
#include <cfenv>
#include <iostream>
#include <set>
#include <stdexcept>
#include "../src/opus_codec.cpp"

int main() try {
  if (std::fesetround(FE_TONEAREST) != 0)
    throw std::runtime_error("round");
  constexpr int N = 12, LM = 1, band = 15, initial_budget = 167;
  const int cap = celt_mode()->logN[band] + LM * 8;
  const int qn = compute_qn(N, initial_budget, (cap >> 1) - 4, cap, 1);
  if (qn != 4)
    throw std::runtime_error("retained5ms-tail qn/domain mismatch");
  const std::array<std::array<float, 2>, 7> directions{{
      {1.f, 0.f}, {0.8f, 0.6f}, {0.6f, 0.8f}, {0.f, 1.f},
      {-0.6f, 0.8f}, {-0.8f, 0.6f}, {-1.f, 0.f}}};
  std::set<int> angles;
  std::set<int> old_endpoint_mismatches;
  int checks = 0;
  for (const auto& direction : directions) {
    for (const int rounding : {-2, 1}) {
      std::array<celt_norm, N> X{}, Y{};
      X[0] = 1.f;
      Y[0] = direction[0];
      Y[1] = direction[1];
      std::array<celt_ener, 2 * celt_default_nb_ebands> energies{};
      energies.fill(1.f);
      std::array<unsigned char, 4096> bytes{};
      ec_ctx ec{};
      ec_enc_init(&ec, bytes.data(), static_cast<opus_uint32>(bytes.size()));
      band_ctx ctx{};
      ctx.encode = 1;
      ctx.resynth = 1;
      ctx.i = band;
      ctx.intensity = celt_default_nb_ebands;
      ctx.ec = &ec;
      ctx.bandE = energies.data();
      ctx.remaining_bits = 631;
      ctx.raw_stereo_itheta = stereo_itheta(X.data(), Y.data(), 1, N);
      ctx.theta_round = rounding;
      int budget = initial_budget, fill = 3;
      split_ctx actual{};
      compute_theta(&ctx, &actual, X.data(), Y.data(), N, &budget, 1, 1, LM, 1, &fill);
      const auto model = celt_theta_model_gains(actual.itheta);
      if (model[0] != actual.imid || model[1] != actual.iside)
        throw std::runtime_error("producer/ACTUALdefault-consumer Q15 mismatch");
      const float actual_mid = (1.f / 32768) * actual.imid;
      const float actual_side = (1.f / 32768) * actual.iside;
      const double m = model[0], s = model[1];
      const double cm = actual_mid, cs = actual_side;
      if ((m * m - s * s) / (m * m + s * s) != (cm * cm - cs * cs) / (cm * cm + cs * cs))
        throw std::runtime_error("model/actual consumer scaling units mismatch");
      if (actual.itheta == 0 || actual.itheta == 16384) {
        const std::array<int, 2> old_raw{
            bitexact_cos(static_cast<opus_int16>(actual.itheta)),
            bitexact_cos(static_cast<opus_int16>(16384 - actual.itheta))};
        if (old_raw[0] != actual.imid || old_raw[1] != actual.iside)
          old_endpoint_mismatches.insert(actual.itheta);
      }
      angles.insert(actual.itheta);
      ++checks;
    }
  }
  if (!angles.contains(0) || !angles.contains(16384) || angles.size() < 5 || old_endpoint_mismatches.size() != 2)
    throw std::runtime_error("required endpoint/interior coverage missing");
  std::cout << "{\"status\":\"PASS_ACTUAL_CONSUMER_GAIN_DOMAIN\",\"checks\":" << checks
            << ",\"N\":12,\"LM\":1,\"band\":15,\"budgetQ3\":167,\"qn\":4,"
               "\"old_raw_endpoint_mismatches\":2,\"new_Q15_and_actual_Q15_scaled_units_equal\":true,\"angles\":[";
  bool comma = false;
  for (const int a : angles) {
    if (comma) std::cout << ',';
    std::cout << a;
    comma = true;
  }
  std::cout << "]}\n";
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
