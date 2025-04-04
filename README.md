# `streamadapter`

`streamadapter` application demonstrates how to export images to the [GStreamer](https://gstreamer.freedesktop.org/) pipeline from [MRTech IFF SDK](https://mr-te.ch/iff-sdk).
It is located in `samples/05_gstreamer` directory of IFF SDK package.
Application comes with example configuration file (`streamadapter.json`) providing the following functionality:

* acquisition from XIMEA camera
* color pre-processing on GPU:
  * black level subtraction
  * histogram calculation
  * white balance
  * demosaicing
  * color correction
  * gamma
  * image format conversion
* automatic control of exposure time and white balance
* image export to the user code

## Usage

GStreamer pipeline description is passed through command line arguments, with `streamadapter` acting as a source.
For example, each incoming image can be dumped to a separate file with this command:

```sh
./streamadapter ! multifilesink
```
