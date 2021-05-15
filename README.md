## Description

Use libav* libraries, which are used by ffmpeg, to transcode video and audio to a certain codec. Also store an `AVFrame` into `.jpg` file format.

## How to run

Install libav* libraries that are included on the `main.cpp`  

Then compile `main.cpp`:

```bash
g++ -std=c++11 -o main main.cpp `pkg-config --cflags --libs libavutil libavcodec libavformat libswscale`
```

Run the program:
```bash
./main myvideo.mp4 myvideo_h265_aac.mp4 15
```
where:
- `main` is the compilation output by g++
- `myvideo.mp4` is the video file that you want to do transcoding on
- `myvideo_h265_aac.mp4` is the output video file that will have new codec
- `15` is the timestamp in second, telling the program at what second we want to capture a frame and store it as a `.jpg` format

This program will output a new video file with `h265` video codec and `aac` audio codec. It will also produce an image in `.jpg` format

You can check the details about the produced video by using this command:
```bash
ffprobe -i myvideo_h265_aac.mp4
```
The output will be something like this:
![image](https://user-images.githubusercontent.com/33726233/118348986-0769a880-b578-11eb-9cab-ac7539396cd0.png)

Here, we can see that `myvideo_h265_aac.mp4` is encoded using `hevc` (h265) and `aac` codec

For more detailed informations about the streams and format, you can use the following command:
```bash
ffprobe -show_format -show_streams myvideo_h265_aac.mp4
```

## Source
I learn about libav* libraries usage and structure from [here](https://github.com/leandromoreira/ffmpeg-libav-tutorial)