// Copyright Erly Inc 2011, All Rights Reserved
// Authors: Walt Lin, Bo Wang
//
// Loosely based on vipsthumbnail.c
//
// Note on EXIF data:
//  There are a number of tags that control how the image should be displayed,
//  among them Exif.Image.Orientation, Exif.Thumbnail.Orientation, and vendor
//  specific tags like Exif.Panasonic.Rotation.  We only read and write the
//  Exif.Image.Orientation tag, which seems to be the most well supported and
//  generally the correct thing to do.  Since we never use the thumbnails
//  embedded in the image we could potentially strip them out if we wanted to
//  save space.  Notably we do not call Exiv2::orientation which looks at all
//  the vendor specific tags, since it may be possible that some library
//  rotated the image and stripped Exif.Image.Orientation but left
//  Exif.Panasonic.Rotation.  Originals taken by panasonics tend to have all
//  three of those tags set.
//
//  We have also seen some photos on Flickr that have Exif.Thumbnail.Orientation
//  and Exif.Panasonic.Rotation but no Exif.Image.Orientation, for example
//  http://www.flickr.com/photos/andrewlin12/3717390632/in/set-72157621264148187/ .
//  We could potentially try to fix these up by stripping the other tags but
//  that's not necessary to get them to display correctly in the browser.
//
// To compile a test program on linux that uses this library:
//  g++ -o myconvert  src/myconvert.cc src/transform.cc   `pkg-config --cflags --libs vips-7.26`  `pkg-config --cflags --libs exiv2`

#include <ctype.h>
#include <math.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <algorithm>
#include <string>
#include <vector>

#include <vips/vips.h>

#include "transform.h"

#define DEBUG 0
#define ORIENTATION ("exif-ifd0-Orientation")

using std::string;

static const char kJpegCompressionFactor[] = ":92";

static const char kOrientationTag[] = "Exif.Image.Orientation";

string SimpleItoa(int x) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", x);
  return string(buf);
}

// Free VipsImages when this object goes out of scope.
class ImageFreer {
 public:
  ImageFreer() {}

  ~ImageFreer() {
    for (unsigned int i = 0; i < v_.size(); i++) {
      if (v_[i] != NULL) {
        g_object_unref(v_[i]);
      }
    }
    v_.clear();
  }

  void add(VipsImage* i) { v_.push_back(i); }

 private:
  std::vector<VipsImage*> v_;
};

// Set an error message from the vips buffer, and clear it.
static void SetFromVipsError(string* out, const char* msg) {
  out->assign(msg);
  out->append(": ");
  out->append(vips_error_buffer());
  vips_error_clear();
}

//ported from libvips/tools/vipsthumbnail.c
static int
get_angle( VipsImage *im )
{
	int angle;
	const char *orientation;

	angle = 0;

	if( vips_image_get_typeof( im, ORIENTATION ) &&
		!vips_image_get_string( im, ORIENTATION, &orientation ) ) {
		if( vips_isprefix( "6", orientation ) )
			angle = 1;
		else if( vips_isprefix( "8", orientation ) )
			angle = 3;
		else if( vips_isprefix( "3", orientation ) )
			angle = 2;

		/* Other values do rotate + mirror, don't bother handling them
		 * though, how common can mirroring be.
		 *
		 * See:
		 *
		 * http://www.80sidea.com/archives/2316
		 */
	}

	return( angle );
}

// Read EXIF data for image in 'path' and return the rotation needed to turn
// it right side up.  Return < 0 upon error, and fill in 'err'.
static int GetEXIFRotationNeeded(VipsImage *image, string *err) {
  int orientation = 0;
  try {
    orientation = get_angle(image);
  } catch (std::exception &e) {
    err->assign("Error calling get_angle");
    return -1;
  }

  // We only expect values of 1, 3, 6, 8, see
  // http://www.impulseadventure.com/photo/exif-orientation.html
  switch (orientation) {
  case 0:    return 0;
  case 1:    return 90;
  case 2:    return 180;
  case 3:    return 270;
  default:
    // Don't error out on bogus values, just assume no rotation needed.
    if (DEBUG) {
      fprintf(stderr, "unexpected orientation value %d\n", orientation);
    }
    return 0;
  }
}

// Calculate shrink factors: return an integer factor to shrink by ( >= 1 ),
// as well as a residual [0,1], so we can shrink in two stages.
static int CalculateShrink(int width, int height,
                           int new_width, int new_height,
                           bool crop, double *residual) {
  double xf = static_cast<double>(width) / std::max(new_width, 1);
  double yf = static_cast<double>(height) / std::max(new_height, 1);
  double factor = crop ? std::min(xf, yf) : std::max(xf, yf);
  factor = std::max(factor, 1.0);
  int shrink = floor(factor);
  if (residual != NULL) {
    *residual = shrink / factor;
  }
  return shrink;
}

// Resize the image, maintaining aspect ratio.  If 'crop' is true, the
// image will be scaled down until one dimension reaches the box and
// then the image will be cropped to reach the exact dimensions
// (keeping it centered); otherwise, the image will be scaled until it
// fits inside the requested box.  Allocate a new VipsImage and return
// it if successful; it will be local to 'in'.
//
// TODO(walt): add sharpening?
static VipsImage* ResizeAndCrop(VipsImage* in, int new_x, int new_y,
                                bool crop) {
  VipsImage* x = in;
  VipsImage* t[4];
  if (im_open_local_array(in, t, 4, "scratch", "p")) {
    return NULL;
  }

  double residual;
  int shrink = CalculateShrink(x->Xsize, x->Ysize, new_x, new_y,
                               crop, &residual);

  if (DEBUG) {
    fprintf(stderr, "resizing image from %dx%d to %dx%d, "
            "crop=%d, shrink=%d, residual scale=%f\n",
            x->Xsize, x->Ysize, new_x, new_y, crop, shrink, residual);
  }

  // First, shrink an integral amount with im_shrink.  Then, do the leftover
  // part with im_affinei using bilinear interpolation.
  VipsInterpolate* interp = vips_interpolate_bilinear_static();
  if (im_shrink(x, t[0], shrink, shrink) ||
      im_affinei_all(t[0], t[1], interp, residual, 0, 0, residual, 0, 0)) {
    return NULL;
  }
  x = t[1];

  if (crop) {
    int width = std::min(x->Xsize, new_x);
    int height = std::min(x->Ysize, new_y);
    int left = (x->Xsize - width + 1) / 2;
    int top = (x->Ysize - height + 1) / 2;
    if (DEBUG) {
      fprintf(stderr, "cropping from %dx%d to %dx%d\n",
              x->Xsize, x->Ysize, new_x, new_y);
    }
    if (im_extract_area(x, t[2], left, top, width, height)) {
      return NULL;
    }
    x = t[2];
  }

  return x;
}

// Rotate the image, returning a new VipsImage that is allocated local
// to 'in'.  Return NULL if there is an error.
VipsImage* Rotate(VipsImage* in, int degrees) {
  VipsImage* tmp = vips_image_new();
  if (tmp == NULL) {
    return NULL;
  }
  vips_object_local(in, tmp);

  int r = 0;
  switch (degrees) {
    case 0:   return in;
    case 90:  r = im_rot90(in, tmp);   break;
    case 180: r = im_rot180(in, tmp);  break;
    case 270: r = im_rot270(in, tmp);  break;
    default:  return NULL;
  }
  return r ? NULL : tmp;
}

int DoTransform(int cols, int rows, bool crop_to_size,
		int rotate_degrees, bool auto_orient,
		const string& src_path, const string& dst_path,
                int* new_width, int* new_height, string* err_msg) {
  ImageFreer freer;

  if (src_path == dst_path) {
    err_msg->assign("dest path cannot be same as source path");
    return -1;
  }

  if (auto_orient && rotate_degrees != 0) {
    err_msg->assign("can't rotate and auto-orient");
    return -1;
  }

  VipsFormatClass* format = vips_format_for_file(src_path.c_str());
  if (format == NULL) {
    SetFromVipsError(err_msg, "could not open file");
    return -1;
  }

  string imgformat = VIPS_OBJECT_CLASS(format)->nickname;
  if (imgformat != "jpeg" && imgformat != "png" && imgformat != "gif" && imgformat != "tiff") {
    err_msg->assign("unsupported image format ");
    err_msg->append(imgformat);
    return -1;
  }

  // Open the input and output images.
  VipsImage *in, *out;
  {
    in = vips_image_new_mode(src_path.c_str(), "rd");
    if (in == NULL) {
      SetFromVipsError(err_msg, "could not open input");
      return -1;
    }
    freer.add(in);

    out = vips_image_new_mode(dst_path.c_str(), "w");
    if (out == NULL) {
      SetFromVipsError(err_msg, "could not open output");
      return -1;
    }

    vips_object_local(in, out);
  }

  if(auto_orient){
    int r = GetEXIFRotationNeeded(in, err_msg);
    if(r < 0) {
      return -1;
    }
    rotate_degrees = r;
  }

  if (rotate_degrees != 0 && rotate_degrees != 90 &&
      rotate_degrees != 180 && rotate_degrees != 270) {
    err_msg->assign("illegal rotate_degrees");
    return -1;
  }

  // Resize and/or crop.
  VipsImage* img = in;
  if (cols > 0 && rows > 0) {
    img = ResizeAndCrop(img, cols, rows, crop_to_size);
    if (img == NULL) {
      SetFromVipsError(err_msg, "resize and crop failed");
      return -1;
    }
  }

  // Rotate.
  if (rotate_degrees > 0) {
    img = Rotate(img, rotate_degrees);
    if (img == NULL) {
      SetFromVipsError(err_msg, "rotate failed");
      return -1;
    }
  }

  // Write the image.
  if (im_copy(img, out)) {
    err_msg->assign("copy failed");
    return -1;
  }

  if (new_width != NULL) *new_width = img->Xsize;
  if (new_height != NULL) *new_height = img->Ysize;

  return 0;
}

void InitTransform(const char* argv0) {
  assert(vips_init(argv0) == 0);
}

int PNGPixel(unsigned char red, unsigned char green, unsigned char blue,
    unsigned char alpha, char** pixelData, size_t* pixelLen, string* err_msg) {
  void* buf = vips_malloc(NULL, 1000);
  if (buf == NULL) {
    err_msg->assign("can't allocate 1k bytes for tmp image");
    return -1;
  }

  VipsImage* tmp = vips_image_new_from_memory(buf, 1, 1, 4, VIPS_FORMAT_UCHAR);
  if (tmp == NULL) {
    err_msg->assign("can't create a vips image");
    vips_free(buf);
    return -1;
  }
  PEL RGBA[4];
  RGBA[0] = red;
  RGBA[1] = green;
  RGBA[2] = blue;
  RGBA[3] = alpha;
  int result = im_draw_flood(tmp, 0, 0, RGBA, NULL);
  if (result != 0) {
    err_msg->assign("draw_rect failed.");
    vips_free(buf);
    return -1;
  }
  tmp->Xres = 2.835017718860743;
  tmp->Yres = 2.835017718860743;
  result = im_vips2bufpng(tmp, NULL, 6, 0, pixelData, pixelLen);
  if (result != 0) {
    err_msg->assign("cannot write as png");
    vips_free(buf);
    return -1;
  }
  vips_free(buf);

  return 0;
}
