##--------------------------------------------------------------------
##  公共设置

##  视频解码器设置
LIB_FFMPEG=-lavutil -lavformat -lavcodec -lswscale -lswresample -lavdevice -lavfilter -lz

##  C++工具链
CXX=g++

##--------------------------------------------------------------------
##  视频文件依赖列表
VIDEO_FILE_LIST = test.mp4

##--------------------------------------------------------------------
##  输出的H264码流文件依赖列表
H264_FILE_LIST = test.h264 

##--------------------------------------------------------------------
##  总目标
all:${H264_FILE_LIST}

##--------------------------------------------------------------------
##  H264输出资源文件依赖
${H264_FILE_LIST}:${VIDEO_FILE_LIST} VideoConv
	@echo "    [H264]  Video to H264"
	@./VideoConv -o ./ ${VIDEO_FILE_LIST}
	@echo "Video To H264 Conv Finish!!"

##--------------------------------------------------------------------
##  转换工具依赖
VideoConv:VideoConv.cpp
	@echo "    [CXX]   VideoConv"
	@${CXX} -o VideoConv VideoConv.cpp ${LIB_FFMPEG} -std=c++11
	@chmod +x VideoConv


##  总体清除
cleanall clean:
	@rm -rf *.o
	@rm -rf VideoConv
	@rm -rf *.h264
	@rm -rf *.vinf
	@echo "Clean Finish!!"

