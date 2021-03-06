// Copyright 2017 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "compressed_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <array>

#include "bit_reader.h"
#include "common.h"
#include "compiler_specific.h"
#include "dc_predictor.h"
#include "dct_util.h"
#include "gauss_blur.h"
#include "huffman_encode.h"
#include "opsin_codec.h"
#include "opsin_params.h"
#include "status.h"

namespace pik {

namespace {

static const int kBlockEdge = 8;
static const int kBlockSize = kBlockEdge * kBlockEdge;

}  // namespace

Image3F AlignImage(const Image3F& in, const size_t N) {
  const size_t block_xsize = DivCeil(in.xsize(), N);
  const size_t block_ysize = DivCeil(in.ysize(), N);
  const size_t xsize = N * block_xsize;
  const size_t ysize = N * block_ysize;
  Image3F out(xsize, ysize);
  int y = 0;
  for (; y < in.ysize(); ++y) {
    for (int c = 0; c < 3; ++c) {
      const float* const PIK_RESTRICT row_in = &in.Row(y)[c][0];
      float* const PIK_RESTRICT row_out = &out.Row(y)[c][0];
      memcpy(row_out, row_in, in.xsize() * sizeof(row_in[0]));
      const int lastcol = in.xsize() - 1;
      const float lastval = row_out[lastcol];
      for (int x = in.xsize(); x < xsize; ++x) {
        row_out[x] = lastval;
      }
    }
  }
  const int lastrow = in.ysize() - 1;
  for (; y < ysize; ++y) {
    for (int c = 0; c < 3; ++c) {
      const float* const PIK_RESTRICT row_in = out.ConstPlaneRow(c, lastrow);
      float* const PIK_RESTRICT row_out = out.PlaneRow(c, y);
      memcpy(row_out, row_in, xsize * sizeof(row_out[0]));
    }
  }
  return out;
}

void CenterOpsinValues(Image3F* img) {
  for (int y = 0; y < img->ysize(); ++y) {
    auto row = img->Row(y);
    for (int x = 0; x < img->xsize(); ++x) {
      for (int c = 0; c < 3; ++c) {
        row[c][x] -= kXybCenter[c];
      }
    }
  }
}

void ApplyColorTransform(const ColorTransform& ctan,
                         const float factor,
                         const ImageF& y_plane,
                         Image3F* coeffs) {
  const int bxsize = coeffs->xsize() / kBlockSize;
  const int bysize = coeffs->ysize();
  const float kYToBScale = 1.0f / 128.0f;
  const float kYToXScale = 1.0f / 256.0f;
  const float factor_b = factor * kYToBScale;
  const float factor_x = factor * kYToXScale;
  for (int y = 0; y < bysize; ++y) {
    const float* const PIK_RESTRICT row_y = y_plane.Row(y);
    float* const PIK_RESTRICT row_x = coeffs->PlaneRow(0, y);
    float* const PIK_RESTRICT row_b = coeffs->PlaneRow(2, y);
    for (int x = 0; x < bxsize; ++x) {
      const int xoff = x * kBlockSize;
      const float ytob_ac =
          factor_b * ctan.ytob_map.Row(y / kTileInBlocks)[x / kTileInBlocks];
      const float ytox_ac =
          factor_x * (ctan.ytox_map.Row(y / kTileInBlocks)[x / kTileInBlocks]
                      - 128);
      row_b[xoff] += factor_b * ctan.ytob_dc * row_y[xoff];
      row_x[xoff] += factor_x * (ctan.ytox_dc - 128) * row_y[xoff];
      for (int k = 1; k < kBlockSize; ++k) {
        row_b[xoff + k] += ytob_ac * row_y[xoff + k];
        row_x[xoff + k] += ytox_ac * row_y[xoff + k];
      }
    }
  }
}

void Adjust2x2ACFromDC(const Image3F& dc, const int direction,
                       Image3F* coeffs) {
  constexpr float kWeight0 = 0.027630534023046f;
  constexpr float kWeight1 = 0.133676439523697f;
  constexpr float kWeight2 = 0.035697385668755f;
  constexpr float kKernel0[3] = { 1.0f, 0.0f, -1.0f };
  constexpr float kKernel1[3] = { kWeight0, kWeight1, kWeight0 };
  constexpr float kKernel2[3] = { kWeight2, 0.0f, -kWeight2 };
  const std::vector<float> kernel0(kKernel0, kKernel0 + 3);
  const std::vector<float> kernel1(kKernel1, kKernel1 + 3);
  const std::vector<float> kernel2(kKernel2, kKernel2 + 3);
  Image3F tmp0 = ConvolveXSampleAndTranspose(dc, kernel0, 1);
  Image3F tmp1 = ConvolveXSampleAndTranspose(dc, kernel1, 1);
  Image3F ac01 = ConvolveXSampleAndTranspose(tmp1, kernel0, 1);
  Image3F ac10 = ConvolveXSampleAndTranspose(tmp0, kernel1, 1);
  Image3F ac11 = ConvolveXSampleAndTranspose(tmp0, kernel2, 1);
  for (int by = 0; by < dc.ysize(); ++by) {
    auto row01 = ac01.ConstRow(by);
    auto row10 = ac10.ConstRow(by);
    auto row11 = ac11.ConstRow(by);
    auto row_out = coeffs->Row(by);
    for (int bx = 0, x = 0; bx < dc.xsize(); ++bx, x += kBlockSize) {
      for (int c = 0; c < 3; ++c) {
        row_out[c][x + 1] += direction * row01[c][bx];
        row_out[c][x + 8] += direction * row10[c][bx];
        row_out[c][x + 9] += direction * row11[c][bx];
      }
    }
  }
}

void ComputePredictionResiduals(const Quantizer& quantizer, Image3F* coeffs,
                                Image3F* predicted_coeffs) {
  Image3W qcoeffs = QuantizeCoeffs(*coeffs, quantizer);
  Image3F dcoeffs = DequantizeCoeffs(qcoeffs, quantizer);
  Image3F prediction_from_dc(coeffs->xsize(), coeffs->ysize(), 0.0f);
  Adjust2x2ACFromDC(DCImage(dcoeffs), 1, &prediction_from_dc);
  SubtractFrom(prediction_from_dc, coeffs);
  qcoeffs = QuantizeCoeffs(*coeffs, quantizer);
  dcoeffs = DequantizeCoeffs(qcoeffs, quantizer);
  AddTo(prediction_from_dc, &dcoeffs);
  Image3F pred = GetPixelSpaceImageFrom2x2Corners(dcoeffs);
  pred = UpSample4x4BlurDCT(pred, 1.5f);
  SubtractFrom(pred, coeffs);
  if (predicted_coeffs) {
    *predicted_coeffs = std::move(pred);
    AddTo(prediction_from_dc, predicted_coeffs);
  }
}

static void ZeroDC(Image3W* coeffs) {
  for (int c = 0; c < 3; c++) {
    for (int y = 0; y < coeffs->ysize(); y++) {
      auto row = coeffs->PlaneRow(c, y);
      for (int x = 0; x < coeffs->xsize(); x += 64) {
        row[x] = 0;
      }
    }
  }
}

QuantizedCoeffs ComputeCoefficients(const CompressParams& params,
                                    const Image3F& opsin,
                                    const Quantizer& quantizer,
                                    const ColorTransform& ctan,
                                    const PikInfo* aux_out) {
  Image3F predicted_coeffs;
  Image3F coeffs = TransposedScaledDCT(opsin);
  ComputePredictionResiduals(quantizer, &coeffs,
                             aux_out != nullptr ? &predicted_coeffs : nullptr);
  ApplyColorTransform(ctan, -1.0f,
                      QuantizeRoundtrip(quantizer, 1, coeffs.plane(1)),
                      &coeffs);
  if (aux_out) {
    ApplyColorTransform(
        ctan, -1.0f, QuantizeRoundtrip(quantizer, 1, predicted_coeffs.plane(1)),
        &predicted_coeffs);
  }
  QuantizedCoeffs qcoeffs;
  qcoeffs.dct = QuantizeCoeffs(coeffs, quantizer);
  if (aux_out) {
    Image3W dct_copy = CopyImage3(qcoeffs.dct);
    ZeroDC(&dct_copy);
    aux_out->DumpCoeffImage("coeff_residuals", dct_copy);
    aux_out->DumpCoeffImage("coeff_predictions",
                            QuantizeCoeffs(predicted_coeffs, quantizer));
  }
  return qcoeffs;
}

void ComputeBlockContextFromDC(const Image3W& coeffs,
                               const Quantizer& quantizer, Image3B* out) {
  const int bxsize = coeffs.xsize() / 64;
  const int bysize = coeffs.ysize();
  const float iquant_base = quantizer.inv_quant_dc();
  for (int c = 0; c < 3; ++c) {
    const float iquant = iquant_base * DequantMatrix()[c * 64];
    const float range = kXybRange[c] / iquant;
    const int kR2Thresh = 10.24f * range * range + 1.0f;
    for (int x = 0; x < bxsize; ++x) {
      out->Row(0)[c][x] = c;
      out->Row(bysize - 1)[c][x] = c;
    }
    for (int y = 1; y + 1 < bysize; ++y) {
      const int16_t* const PIK_RESTRICT row_t = coeffs.ConstPlaneRow(c, y - 1);
      const int16_t* const PIK_RESTRICT row_m = coeffs.ConstPlaneRow(c, y);
      const int16_t* const PIK_RESTRICT row_b = coeffs.ConstPlaneRow(c, y + 1);
      uint8_t* const PIK_RESTRICT row_out = out->PlaneRow(c, y);
      row_out[0] = row_out[bxsize - 1] = c;
      for (int bx = 1; bx + 1 < bxsize; ++bx) {
        const int x = bx * 64;
        const int16_t val_tl = row_t[x - 64];
        const int16_t val_tm = row_t[x];
        const int16_t val_tr = row_t[x + 64];
        const int16_t val_ml = row_m[x - 64];
        const int16_t val_mr = row_m[x + 64];
        const int16_t val_bl = row_b[x - 64];
        const int16_t val_bm = row_b[x];
        const int16_t val_br = row_b[x + 64];
        const int dx = (3 * (val_tr - val_tl + val_br - val_bl) +
                        10 * (val_mr - val_ml));
        const int dy = (3 * (val_bl - val_tl + val_br - val_tr) +
                        10 * (val_bm - val_tm));
        const int dx2 = dx * dx;
        const int dy2 = dy * dy;
        const int dxdy = std::abs(2 * dx * dy);
        const int r2 = dx2 + dy2;
        const int d2 = dy2 - dx2;
        if (r2 < kR2Thresh) {
          row_out[bx] = c;
        } else if (d2 < -dxdy) {
          row_out[bx] = 3;
        } else if (d2 > dxdy) {
          row_out[bx] = 5;
        } else {
          row_out[bx] = 4;
        }
      }
    }
  }
}

std::string EncodeColorMap(const Image<int>& ac_map,
                           const int dc_val,
                           PikImageSizeInfo* info) {
  const size_t max_out_size = ac_map.xsize() * ac_map.ysize() + 1024;
  std::string output(max_out_size, 0);
  size_t storage_ix = 0;
  uint8_t* storage = reinterpret_cast<uint8_t*>(&output[0]);
  storage[0] = 0;
  std::vector<uint32_t> histogram(256);
  ++histogram[dc_val];
  for (int y = 0; y < ac_map.ysize(); ++y) {
    for (int x = 0; x < ac_map.xsize(); ++x) {
      ++histogram[ac_map.Row(y)[x]];
    }
  }
  std::vector<uint8_t> bit_depths(256);
  std::vector<uint16_t> bit_codes(256);
  BuildAndStoreHuffmanTree(histogram.data(), histogram.size(),
                           bit_depths.data(), bit_codes.data(),
                           &storage_ix, storage);
  const size_t histo_bits = storage_ix;
  WriteBits(bit_depths[dc_val], bit_codes[dc_val], &storage_ix, storage);
  for (int y = 0; y < ac_map.ysize(); ++y) {
    const int* const PIK_RESTRICT row = ac_map.Row(y);
    for (int x = 0; x < ac_map.xsize(); ++x) {
      WriteBits(bit_depths[row[x]], bit_codes[row[x]], &storage_ix, storage);
    }
  }
  WriteZeroesToByteBoundary(&storage_ix, storage);
  PIK_ASSERT((storage_ix >> 3) <= output.size());
  output.resize(storage_ix >> 3);
  if (info) {
    info->histogram_size = histo_bits >> 3;
    info->entropy_coded_bits = storage_ix - histo_bits;
    info->total_size += output.size();
  }
  return output;
}

std::string EncodeToBitstream(const QuantizedCoeffs& qcoeffs,
                             const Quantizer& quantizer,
                              const NoiseParams& noise_params,
                              const ColorTransform& ctan,
                              bool fast_mode,
                              PikInfo* info) {
  const Image3W& qdct = qcoeffs.dct;
  const size_t x_tilesize =
      DivCeil(qdct.xsize(), kSupertileInBlocks * kBlockSize);
  const size_t y_tilesize = DivCeil(qdct.ysize(), kSupertileInBlocks);
  PikImageSizeInfo* ctan_info = info ? &info->layers[kLayerCtan] : nullptr;
  std::string ctan_code =
      EncodeColorMap(ctan.ytob_map, ctan.ytob_dc, ctan_info) +
      EncodeColorMap(ctan.ytox_map, ctan.ytox_dc, ctan_info);
  PikImageSizeInfo* quant_info = info ? &info->layers[kLayerQuant] : nullptr;
  PikImageSizeInfo* dc_info = info ? &info->layers[kLayerDC] : nullptr;
  std::string noise_code = EncodeNoise(noise_params);
  std::string quant_code = quantizer.Encode(quant_info);
  std::string dc_code = "";
  Image3W predicted_dc(qdct.xsize() / kBlockSize, qdct.ysize());
  Image3B block_ctx(qdct.xsize() / kBlockSize, qdct.ysize());
  for (int y = 0; y < y_tilesize; y++) {
    for (int x = 0; x < x_tilesize; x++) {
      Image3W predicted_dc_tile =
          Window(&predicted_dc, x * kSupertileInBlocks, y * kSupertileInBlocks,
                 kSupertileInBlocks, kSupertileInBlocks);
      ConstWrapper<Image3W> qdct_tile = ConstWindow(
          qdct, x * kSupertileInBlocks * kBlockSize, y * kSupertileInBlocks,
          kSupertileInBlocks * kBlockSize, kSupertileInBlocks);
      PredictDCTile(qdct_tile.get(), &predicted_dc_tile);
      dc_code += EncodeImage(predicted_dc_tile, 1, dc_info);
      Image3B block_ctx_tile =
          Window(&block_ctx, x * kSupertileInBlocks, y * kSupertileInBlocks,
                 kSupertileInBlocks, kSupertileInBlocks);
      ComputeBlockContextFromDC(qdct_tile.get(), quantizer, &block_ctx_tile);
    }
  }
  std::string ac_code = fast_mode ?
      EncodeACFast(qdct, block_ctx, info) :
      EncodeAC(qdct, block_ctx, info);
  std::string out = ctan_code + noise_code + quant_code + dc_code + ac_code;
  if (info) {
    info->layers[kLayerHeader].total_size += noise_code.size();
    if (out.size() % 4) {
      info->layers[kLayerHeader].total_size += 4 - (out.size() % 4);
    }
  }
  return PadTo4Bytes(out);
}

bool DecodeColorMap(BitReader* br, Image<int>* ac_map, int* dc_val) {
  HuffmanDecodingData entropy;
  if (!entropy.ReadFromBitStream(br)) {
    return PIK_FAILURE("Invalid histogram data.");
  }
  HuffmanDecoder decoder;
  br->FillBitBuffer();
  *dc_val = decoder.ReadSymbol(entropy, br);
  for (int y = 0; y < ac_map->ysize(); ++y) {
    int* const PIK_RESTRICT row = ac_map->Row(y);
    for (int x = 0; x < ac_map->xsize(); ++x) {
      br->FillBitBuffer();
      row[x] = decoder.ReadSymbol(entropy, br);
    }
  }
  br->JumpToByteBoundary();
  return true;
}

bool DecodeFromBitstream(const uint8_t* data, const size_t data_size,
                         const size_t xsize, const size_t ysize,
                         ColorTransform* ctan,
                         NoiseParams* noise_params,
                         Quantizer* quantizer,
                         QuantizedCoeffs* qcoeffs,
                         size_t* compressed_size) {
  const size_t x_blocksize = DivCeil(xsize, kBlockEdge);
  const size_t y_blocksize = DivCeil(ysize, kBlockEdge);
  const size_t x_stilesize = DivCeil(x_blocksize, kSupertileInBlocks);
  const size_t y_stilesize = DivCeil(y_blocksize, kSupertileInBlocks);
  if (data_size == 0) {
    return PIK_FAILURE("Empty compressed data.");
  }
  qcoeffs->dct = Image3W(x_blocksize * kBlockSize, y_blocksize);
  BitReader br(data, data_size & ~3);
  DecodeColorMap(&br, &ctan->ytob_map, &ctan->ytob_dc);
  DecodeColorMap(&br, &ctan->ytox_map, &ctan->ytox_dc);
  if (!DecodeNoise(&br, noise_params)) {
    return PIK_FAILURE("noise decoding failed.");
  }
  if (!quantizer->Decode(&br)) {
    return PIK_FAILURE("quantizer Decode failed.");
  }
  Image3B block_ctx(x_blocksize, y_blocksize);
  for (int y = 0; y < y_stilesize; y++) {
    for (int x = 0; x < x_stilesize; x++) {
      Image3W qdct_tile = Window(
          &qcoeffs->dct,
          x * kSupertileInBlocks * kBlockSize,
          y * kSupertileInBlocks,
          kSupertileInBlocks * kBlockSize, kSupertileInBlocks);
      if (!DecodeImage(&br, kBlockSize, &qdct_tile)) {
        return PIK_FAILURE("DecodeImage failed.");
      }
      UnpredictDCTile(&qdct_tile);
      Image3B block_ctx_tile =
          Window(&block_ctx, x * kSupertileInBlocks, y * kSupertileInBlocks,
                 kSupertileInBlocks, kSupertileInBlocks);
      ComputeBlockContextFromDC(qdct_tile, *quantizer, &block_ctx_tile);
    }
  }
  if (!DecodeAC(block_ctx, &br, &qcoeffs->dct)) {
    return PIK_FAILURE("DecodeAC failed.");
  }
  *compressed_size = br.Position();
  return true;
}

Image3F ReconOpsinImage(const QuantizedCoeffs& qcoeffs,
                        const Quantizer& quantizer, const ColorTransform& ctan,
                        Image3F* transposed_scaled_dct) {
  Image3F dcoeffs = DequantizeCoeffs(qcoeffs.dct, quantizer);
  ApplyColorTransform(ctan, 1.0f, dcoeffs.plane(1), &dcoeffs);
  Adjust2x2ACFromDC(DCImage(dcoeffs), 1, &dcoeffs);
  Image3F pred = GetPixelSpaceImageFrom2x2Corners(dcoeffs);
  pred = UpSample4x4BlurDCT(pred, 1.5f);
  AddTo(pred, &dcoeffs);
  if (transposed_scaled_dct != nullptr) {
    *transposed_scaled_dct = CopyImage3(dcoeffs);
  }
  return TransposedScaledIDCT(dcoeffs);
}

}  // namespace pik
