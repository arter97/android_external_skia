/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef Sk4pxXfermode_DEFINED
#define Sk4pxXfermode_DEFINED

#include "Sk4px.h"

// This file is possibly included into multiple .cpp files.
// Each gets its own independent instantiation by wrapping in an anonymous namespace.
namespace {

#define XFERMODE(Name)                                                    \
    struct Name {                                                         \
        static Sk4px Xfer(const Sk4px&, const Sk4px&);                    \
        static const SkXfermode::Mode kMode = SkXfermode::k##Name##_Mode; \
    };                                                                    \
    inline Sk4px Name::Xfer(const Sk4px& s, const Sk4px& d)

XFERMODE(Clear) { return Sk4px::DupPMColor(0); }
XFERMODE(Src)   { return s; }
XFERMODE(Dst)   { return d; }
XFERMODE(SrcIn)   { return     s.approxMulDiv255(d.alphas()      ); }
XFERMODE(SrcOut)  { return     s.approxMulDiv255(d.alphas().inv()); }
XFERMODE(SrcOver) { return s + d.approxMulDiv255(s.alphas().inv()); }
XFERMODE(DstIn)   { return SrcIn  ::Xfer(d,s); }
XFERMODE(DstOut)  { return SrcOut ::Xfer(d,s); }
XFERMODE(DstOver) { return SrcOver::Xfer(d,s); }

// [ S * Da + (1 - Sa) * D]
XFERMODE(SrcATop) { return (s * d.alphas() + d * s.alphas().inv()).div255(); }
XFERMODE(DstATop) { return SrcATop::Xfer(d,s); }
//[ S * (1 - Da) + (1 - Sa) * D ]
XFERMODE(Xor) { return (s * d.alphas().inv() + d * s.alphas().inv()).div255(); }
// [S + D ]
XFERMODE(Plus) { return s.saturatedAdd(d); }
// [S * D ]
XFERMODE(Modulate) { return s.approxMulDiv255(d); }
// [S + D - S * D]
XFERMODE(Screen) {
    // Doing the math as S + (1-S)*D or S + (D - S*D) means the add and subtract can be done
    // in 8-bit space without overflow.  S + (1-S)*D is a touch faster because inv() is cheap.
    return s + d.approxMulDiv255(s.inv());
}
XFERMODE(Multiply) { return (s * d.alphas().inv() + d * s.alphas().inv() + s*d).div255(); }
// [ Sa + Da - Sa*Da, Sc + Dc - 2*min(Sc*Da, Dc*Sa) ]  (And notice Sa*Da == min(Sa*Da, Da*Sa).)
XFERMODE(Difference) {
    auto m = Sk4px::Wide::Min(s * d.alphas(), d * s.alphas()).div255();
    // There's no chance of underflow, and if we subtract m before adding s+d, no overflow.
    return (s - m) + (d - m.zeroAlphas());
}
// [ Sa + Da - Sa*Da, Sc + Dc - 2*Sc*Dc ]
XFERMODE(Exclusion) {
    auto p = s.approxMulDiv255(d);
    // There's no chance of underflow, and if we subtract p before adding src+dst, no overflow.
    return (s - p) + (d - p.zeroAlphas());
}

// We take care to use exact math for these next few modes where alphas
// and colors are calculated using significantly different math.  We need
// to preserve premul invariants, and exact math makes this easier.
//
// TODO: Some of these implementations might be able to be sped up a bit
// while maintaining exact math, but let's follow up with that.

XFERMODE(HardLight) {
    auto sa = s.alphas(),
         da = d.alphas();

    auto srcover = s + (d * sa.inv()).div255();

    auto isLite = ((sa-s) < s).widenLoHi();

    auto lite = sa*da - ((da-d)*(sa-s) << 1),
         dark = s*d << 1,
         both = s*da.inv() + d*sa.inv();

    auto alphas = srcover;
    auto colors = (both + isLite.thenElse(lite, dark)).div255();
    return alphas.zeroColors() + colors.zeroAlphas();
}
XFERMODE(Overlay) { return HardLight::Xfer(d,s); }

XFERMODE(Darken) {
    auto sa = s.alphas(),
         da = d.alphas();

    auto sda = (s*da).div255(),
         dsa = (d*sa).div255();

    auto srcover = s + (d * sa.inv()).div255(),
         dstover = d + (s * da.inv()).div255();
    auto alphas = srcover,
         colors = (sda < dsa).thenElse(srcover, dstover);
    return alphas.zeroColors() + colors.zeroAlphas();
}
XFERMODE(Lighten) {
    auto sa = s.alphas(),
         da = d.alphas();

    auto sda = (s*da).div255(),
         dsa = (d*sa).div255();

    auto srcover = s + (d * sa.inv()).div255(),
         dstover = d + (s * da.inv()).div255();
    auto alphas = srcover,
         colors = (dsa < sda).thenElse(srcover, dstover);
    return alphas.zeroColors() + colors.zeroAlphas();
}

#undef XFERMODE

// A reasonable fallback mode for doing AA is to simply apply the transfermode first,
// then linearly interpolate the AA.
template <typename Mode>
static Sk4px xfer_aa(const Sk4px& s, const Sk4px& d, const Sk4px& aa) {
    Sk4px bw = Mode::Xfer(s, d);
    return (bw * aa + d * aa.inv()).div255();
}

// For some transfermodes we specialize AA, either for correctness or performance.
#define XFERMODE_AA(Name) \
    template <> Sk4px xfer_aa<Name>(const Sk4px& s, const Sk4px& d, const Sk4px& aa)

// Plus' clamp needs to happen after AA.  skia:3852
XFERMODE_AA(Plus) {  // [ clamp( (1-AA)D + (AA)(S+D) ) == clamp(D + AA*S) ]
    return d.saturatedAdd(s.approxMulDiv255(aa));
}

#undef XFERMODE_AA

template <typename ProcType>
class SkT4pxXfermode : public SkProcCoeffXfermode {
public:
    static SkProcCoeffXfermode* Create(const ProcCoeff& rec) {
        return SkNEW_ARGS(SkT4pxXfermode, (rec));
    }

    void xfer32(SkPMColor dst[], const SkPMColor src[], int n, const SkAlpha aa[]) const override {
        if (NULL == aa) {
            Sk4px::MapDstSrc(n, dst, src, [&](const Sk4px& dst4, const Sk4px& src4) {
                return ProcType::Xfer(src4, dst4);
            });
        } else {
            Sk4px::MapDstSrcAlpha(n, dst, src, aa,
                    [&](const Sk4px& dst4, const Sk4px& src4, const Sk4px& alpha) {
                return xfer_aa<ProcType>(src4, dst4, alpha);
            });
        }
    }

private:
    SkT4pxXfermode(const ProcCoeff& rec) : SkProcCoeffXfermode(rec, ProcType::kMode) {}

    typedef SkProcCoeffXfermode INHERITED;
};

static SkProcCoeffXfermode* SkCreate4pxXfermode(const ProcCoeff& rec, SkXfermode::Mode mode) {
#if !defined(SK_CPU_ARM32) || defined(SK_ARM_HAS_NEON)
    switch (mode) {
        case SkXfermode::kClear_Mode:      return SkT4pxXfermode<Clear>::Create(rec);
        case SkXfermode::kSrc_Mode:        return SkT4pxXfermode<Src>::Create(rec);
        case SkXfermode::kDst_Mode:        return SkT4pxXfermode<Dst>::Create(rec);
        case SkXfermode::kSrcOver_Mode:    return SkT4pxXfermode<SrcOver>::Create(rec);
        case SkXfermode::kDstOver_Mode:    return SkT4pxXfermode<DstOver>::Create(rec);
        case SkXfermode::kSrcIn_Mode:      return SkT4pxXfermode<SrcIn>::Create(rec);
        case SkXfermode::kDstIn_Mode:      return SkT4pxXfermode<DstIn>::Create(rec);
        case SkXfermode::kSrcOut_Mode:     return SkT4pxXfermode<SrcOut>::Create(rec);
        case SkXfermode::kDstOut_Mode:     return SkT4pxXfermode<DstOut>::Create(rec);
        case SkXfermode::kSrcATop_Mode:    return SkT4pxXfermode<SrcATop>::Create(rec);
        case SkXfermode::kDstATop_Mode:    return SkT4pxXfermode<DstATop>::Create(rec);
        case SkXfermode::kXor_Mode:        return SkT4pxXfermode<Xor>::Create(rec);
        case SkXfermode::kPlus_Mode:       return SkT4pxXfermode<Plus>::Create(rec);
        case SkXfermode::kModulate_Mode:   return SkT4pxXfermode<Modulate>::Create(rec);
        case SkXfermode::kScreen_Mode:     return SkT4pxXfermode<Screen>::Create(rec);
        case SkXfermode::kMultiply_Mode:   return SkT4pxXfermode<Multiply>::Create(rec);
        case SkXfermode::kDifference_Mode: return SkT4pxXfermode<Difference>::Create(rec);
        case SkXfermode::kExclusion_Mode:  return SkT4pxXfermode<Exclusion>::Create(rec);
#if !defined(SK_SUPPORT_LEGACY_XFERMODES)  // For staging in Chrome (layout tests).
        case SkXfermode::kHardLight_Mode:  return SkT4pxXfermode<HardLight>::Create(rec);
        case SkXfermode::kOverlay_Mode:    return SkT4pxXfermode<Overlay>::Create(rec);
        case SkXfermode::kDarken_Mode:     return SkT4pxXfermode<Darken>::Create(rec);
        case SkXfermode::kLighten_Mode:    return SkT4pxXfermode<Lighten>::Create(rec);
#endif
        default: break;
    }
#endif
    return nullptr;
}

} // namespace

#endif//Sk4pxXfermode_DEFINED
