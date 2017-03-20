/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsWEBPDecoder_h__
#define nsWEBPDecoder_h__

#include "Decoder.h"
#include "Downscaler.h"
#include "mozilla/Maybe.h"

extern "C" {
#include "webp/decode.h"
}

namespace mozilla {
namespace image {
class RasterImage;

//////////////////////////////////////////////////////////////////////
// nsWEBPDecoder Definition

class nsWEBPDecoder : public Decoder
{
public:
  nsWEBPDecoder(RasterImage* aImage);
  ~nsWEBPDecoder() override;

  nsresult SetTargetSize(const nsIntSize& aSize) override;

  void InitInternal() override;
  void WriteInternal(const char* aBuffer, uint32_t aCount) override;
  void FinishInternal() override;
private:
  Maybe<Downscaler> mDownscaler;
  WebPIDecoder *mDecoder;
  uint8_t *mData;          // Pointer to WebP-decoded data.
  int mDataRowPadding;     // Buffer row stride minus row data width in bytes.
  int mPreviousLastLine;   // Last image scan-line read so far.
  bool mContextInitialized;

};

} // namespace image
} // namespace mozilla

#endif // nsWEBPDecoder_h__
