/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ImageLogging.h"
#include "nsWEBPDecoder.h"

#include "gfxColor.h"
#include "gfxPlatform.h"

namespace mozilla {
namespace image {

#if defined(PR_LOGGING)
static PRLogModuleInfo *gWEBPLog = PR_NewLogModule("WEBPDecoder");
static PRLogModuleInfo *gWEBPDecoderAccountingLog =
                        PR_NewLogModule("WEBPDecoderAccounting");
#else
#define gWEBPlog
#define gWEBPDecoderAccountingLog
#endif

nsWEBPDecoder::nsWEBPDecoder(RasterImage* aImage)
 : Decoder(aImage)
 , mDecoder(nullptr)
 , mData(nullptr)
 , mDataRowPadding(0)
 , mPreviousLastLine(0)
 , mContextInitialized(false)
{
  PR_LOG(gWEBPDecoderAccountingLog, PR_LOG_DEBUG,
         ("nsWEBPDecoder::nsWEBPDecoder: Creating WEBP decoder %p",
          this));
}

nsWEBPDecoder::~nsWEBPDecoder()
{
  PR_LOG(gWEBPDecoderAccountingLog, PR_LOG_DEBUG,
         ("nsWEBPDecoder::~nsWEBPDecoder: Destroying WEBP decoder %p",
          this));

  // It is safe to pass nullptr to WebPIDelete().
  WebPIDelete(mDecoder);
}

nsresult
nsWEBPDecoder::SetTargetSize(const nsIntSize& aSize)
{
  // Calling this method equals to requesting a downscale-during-decode.

  // Make sure the size is reasonable.
  if (MOZ_UNLIKELY(aSize.width <= 0 || aSize.height <= 0)) {
    return NS_ERROR_FAILURE;
  }

  // Create a downscaler through which output will be filtered.
  mDownscaler.emplace(aSize);

  return NS_OK;
}

void
nsWEBPDecoder::InitInternal()
{
  mDecoder = WebPINewRGB(MODE_rgbA, nullptr, 0, 0);

  if (!mDecoder) {
    PostDecoderError(NS_ERROR_FAILURE);
    return;
  }
}

void
nsWEBPDecoder::FinishInternal()
{
  MOZ_ASSERT(!HasError(), "Shouldn't call FinishInternal after error!");

  // We should never make multiple frames
  MOZ_ASSERT(GetFrameCount() <= 1, "Multiple WebP frames?");

  // Send notifications if appropriate
  if (!IsSizeDecode() && (GetFrameCount() == 1)) {
    PostFrameStop();
    PostDecodeDone();
  }
}

void
nsWEBPDecoder::WriteInternal(const char *aBuffer, uint32_t aCount)
{
  MOZ_ASSERT(!HasError(), "Shouldn't call WriteInternal after error!");

  VP8StatusCode rv = WebPIAppend(mDecoder,
                                reinterpret_cast<const uint8_t*>(aBuffer),
                                aCount);

  if (rv == VP8_STATUS_OUT_OF_MEMORY) {
    PostDecoderError(NS_ERROR_OUT_OF_MEMORY);
    return;
  } else if (rv == VP8_STATUS_INVALID_PARAM ||
             rv == VP8_STATUS_BITSTREAM_ERROR) {
    PostDataError();
    return;
  } else if (rv == VP8_STATUS_UNSUPPORTED_FEATURE ||
             rv == VP8_STATUS_USER_ABORT) {
    PostDecoderError(NS_ERROR_FAILURE);
    return;
  }

  // Catch any remaining erroneous return value.
  if (rv != VP8_STATUS_OK && rv != VP8_STATUS_SUSPENDED) {
    PostDecoderError(NS_ERROR_FAILURE);
    return;
  }

  int lastLineRead = -1;
  int height = 0;
  int width = 0;
  int stride = 0;

  mData = WebPIDecGetRGB(mDecoder, &lastLineRead, &width, &height, &stride);

  if (lastLineRead == -1 || !mData) {
    return;
  }

  if (!HasSize()) {
    if (width <= 0 || height <= 0) {
      PostDataError();
      return;
    }

    // From https://developers.google.com/speed/webp/faq
    // "WebP is bitstream-compatible with VP8 and uses 14 bits for width and
    // height. The maximum pixel dimensions of a WebP image is 16383 x 16383."
    MOZ_ASSERT(width <= 16383 && height <= 16383,
               "Unexpected WebP image dimensions.");

    // The casts will prevent warnings should Pale Moon ever be compiled in
    // an environment where sizeof(int) > 4. Truncation here is not an issue
    // and will be caught in a later check when saved and returned size values
    // are compared; the size values should never be so large they'd become
    // truncated here anyway.
    PostSize(static_cast<int32_t>(width), static_cast<int32_t>(height));
  }

  // Size-only decode ends here.
  if (IsSizeDecode()) {
    return;
  }

  // Make sure that the limits used for buffer access are consistent.
  if (GetSize().width != width || GetSize().height != height) {
    PostDecoderError(NS_ERROR_FAILURE);
    return;
  }

  if (!mContextInitialized) {
    // The only valid format for WebP decoding for both alpha and non-alpha
    // images is BGRA, where Opaque images have an A of 255.
    // Assume transparency for all images.
    // XXX: This could be compositor-optimized by doing a one-time check for
    // all-255 alpha pixels, but that might interfere with progressive
    // decoding. Probably not worth it?
    PostHasTransparency();

    // Initialize the downscaler if downscale-during-decode has been requested.
    if (mDownscaler) {
      nsresult rv = mDownscaler->BeginFrame(GetSize(),
                                            mImageData,
                                            /* aHasAlpha = */ true);

      if (NS_FAILED(rv)) {
        PostDecoderError(NS_ERROR_FAILURE);
        return;
      }
    }

    mBufferRowPadding = stride - width * 4;

    mContextInitialized = true;
  }

  // If nothing has been decoded in this call, wait for more data.
  if (lastLineRead <= mPreviousLastLine) {
    return;
  }

  // Make sure the buffer won't be overshot should the decoder lie.
  if (lastLineRead >= height) {
    // Simply return if everything of interest has already been decoded.
    if (mPreviousLastLine == height - 1) {
      return;
    }

    // There still are some lines missing at the bottom of the image, clip the
    // larger decoded data to that which will fit.
    lastLineRead = height - 1;
  }

  // Transfer the decoded data to output buffer.
  // From: RGBA (byte-order; pre-multiplied alpha)
  // To:   BGRA (word-order; pre-multiplied alpha)
  if (mDownscaler) {
    for (int iLine = mPreviousLastLine; iLine <= lastLineRead; ++iLine) {
      uint32_t* dataOut =
          reinterpret_cast<uint32_t*>(mDownscaler->RowBuffer());

      for (int iPixel = 0; iPixel < width; ++iPixel) {
        *dataOut = gfxPackedPixelNoPreMultiply(*(mData + 3),
                                               *(mData),
                                               *(mData + 1),
                                               *(mData + 2));

        mData += 4;
        ++dataOut;
      }

      mDownscaler->CommitRow();

      mData += mDataRowPadding;
    }
  } else {
    uint32_t* dataOut =
        reinterpret_cast<uint32_t*>(mImageData) + mPreviousLastLine * width;

    for (int iLine = mPreviousLastLine; iLine <= lastLineRead; ++iLine) {
      for (int iPixel = 0; iPixel < width; ++iPixel) {
        *dataOut = gfxPackedPixelNoPreMultiply(*(mData + 3),
                                               *(mData),
                                               *(mData + 1),
                                               *(mData + 2));

        mData += 4;
        ++dataOut;
      }

      mData += mDataRowPadding;
    }
  }

  // Invalidate the appropriate part of the output image.
  PostInvalidation(nsIntRect(0, mPreviousLastLine, width, lastLineRead),
      mDownscaler ? Some(mDownscaler->TakeInvalidRect()) : Nothing());

  mPreviousLastLine = lastLineRead;
  return;
}

} // namespace imagelib
} // namespace mozilla
