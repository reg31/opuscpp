#include "../src/opus_codec.cpp"
#include <cstdio>
#include <vector>
extern "C" int official_transient_analysis(const float*, int, int, float*, int*, int, int*, float, float);
int main() {
  int checked=0, failures=0, attacks=0;
  for (int lm=0; lm<4; ++lm) for (int channels: {1,2}) for (int shape=0; shape<5; ++shape)
    for (bool allow_weak: {false,true}) for (bool tonal: {false,true}) {
      const int len=(120<<lm)+120;
      std::vector<float> pcm(static_cast<std::size_t>(channels*len));
      unsigned random=7;
      for (int c=0;c<channels;++c) for (int i=0;i<len;++i) {
        random=random*1664525u+1013904223u;
        float noise=static_cast<float>(static_cast<int>(random>>16)-32768);
        float gain=shape==0 ? 0.f : shape==1 ? 1.f : shape==2 ? (i>len/2 ? 1.f:.0001f)
          : shape==3 ? (i==len/2+c*3 ? 1.f:0.f) : (i>len/3 && i<len/3+12 ? 1.f:.001f);
        pcm[c*len+i]=gain*noise;
      }
      float got_est=0,ref_est=0;
      int got_ch=0,ref_ch=0,ref_weak=0;
      bool got_weak=false;
      float tone_freq=tonal?.01f:.1f,toneishness=tonal?1.f:0.f;
      int got=celt_transient_analysis(pcm.data(),len,channels,&got_est,&got_ch,allow_weak,&got_weak,tone_freq,toneishness);
      int ref=official_transient_analysis(pcm.data(),len,channels,&ref_est,&ref_ch,allow_weak,&ref_weak,tone_freq,toneishness);
      ++checked;attacks+=ref!=0;
      if(got!=ref || got_est!=ref_est || got_ch!=ref_ch || got_weak!=static_cast<bool>(ref_weak)) {
        if(failures<5) std::printf("mismatch lm=%d ch=%d shape=%d allow_weak=%d tonal=%d transient=%d/%d estimate=%.9g/%.9g channel=%d/%d weak=%d/%d\n",
          lm,channels,shape,allow_weak,tonal,got,ref,got_est,ref_est,got_ch,ref_ch,got_weak,ref_weak);
        ++failures;
      }
    }
  std::printf("transient_parity cases=%d official_attacks=%d failures=%d\n",checked,attacks,failures);
  return failures || attacks==0;
}
