/**********************************************************************

    程序名称：将带有H264视频流的带壳视频文件分离出纯H264流
    程序版本：REV 0.5
    设计编写：rainhenry
    创建日期：20210331

    版本修订：
        REV 0.1  20210331  rainhenry   创建文档
        REV 0.2  20210406  rainhenry   增加生成视频信息文件.vinf
                                       增加控制输出目录功能
        REV 0.3  20210408  rainhenry   增加打印当前正在处理的视频文件名字
        REV 0.4  20210423  rainhenry   不丢弃非关键帧，并增加检查
        REV 0.5  20210714  rainhenry   将输出信息文件增加每一帧的数据字节长度

    设计说明
        将带有H264视频流的带壳视频文件分离出纯H264流,当不是H264的流的时候
    程序会执行失败,并报错

    视频信息文件格式
        长       unsigned int
        宽       unsigned int
        帧率     float
        总帧数   unsigned long
        以上信息全部用空格分隔

    关键的NAL帧头说明
        00 00 00 01 67是SPS
        00 00 00 01 68是PPS
        00 00 00 01 06是SEI
        00 00 00 01 65是I帧（IDR关键帧）
        00 00 00 01 41是非关键帧（No-IDR）
        00 00 00 01 01是B帧
        00 00 00 01 09是AU Delimiter

    关于SEI段的理解
        目前我理解是这样的，00 00 00 01 06这个是个NAL头部，表示此区域是SEI区域，
        然后后面的05表示的是遵循user_data_unregistered()语法（至于是什么语法我也不知道），
        然后后面的字节表示整个SEI的数据区的长度，但是这个长度的表示方法很特殊，
        如果第一个长度字节为0xFF，那么就要多计算一个长度字节，就是下一个字节也是长度，
        表示将二者加到一起。比如00 00 00 01 06 2F，由于第一个长度字节是0x2F，
        所以不等于0xFF，所以长度就为0x2F即十进制的47个字节
        （其中前16个字节为UUID，后31个字节为用户信息，也就是那堆ASCII文本）。
        但是如果是00 00 00 01 06 05 FF FF AC，长度字节1和2都为0xFF，
        所以SEI的总长度应该为0xFF+0xFF+0xAC，即0x2AA，即十进制的682字节
        （其中包含固定16字节长度的UUID和666字节的用户信息）。
        然后SEI的结尾是一个固定的0x80,这个叫rbsp trailing bits

    参考资源
        [1]  https://www.jianshu.com/p/ab4eb019f8f3          参考代码
        [2]  https://wenku.baidu.com/view/b01f78d4cc22bcd127ff0c55.html        常用NAL头介绍
        [3]  https://zhuanlan.zhihu.com/p/71928833         H264整体介绍
        [4]  https://zhuanlan.zhihu.com/p/33720871         SEI的提取方法
        [5]  https://blog.csdn.net/mincheat/article/details/48713047
        [6]  https://blog.csdn.net/yangguoyu8023/article/details/107855698
        [7]  https://blog.csdn.net/jefry_xdz/article/details/8461343
        [8]  https://blog.csdn.net/abcjennifer/article/details/6577934      I帧,P帧,B帧
        [9]  https://blog.csdn.net/evsqiezi/article/details/8492593       NALU/SPS/PPS
        [10] https://blog.csdn.net/y601500359/article/details/80943990    SEI更详细的说明,含代码

    帧标志说明
        以下信息定义在 libavcodec/packet.h 文件中
        #define AV_PKT_FLAG_KEY     0x0001 ///< The packet contains a keyframe
                                           ///< 这个是关键帧
        #define AV_PKT_FLAG_CORRUPT 0x0002 ///< The packet content is corrupted
                                           ///< 数据包内容被破坏
         **
         * Flag is used to discard packets which are required to maintain valid
         * decoder state but are not required for output and should be dropped
         * after decoding.
         * 标志用于丢弃需要保持有效解码器状态但不需要输出的包，解码后应该丢弃这些包。
         ** 
        #define AV_PKT_FLAG_DISCARD   0x0004
        **
         * The packet comes from a trusted source.
         * 数据包来自一个可信的来源。
         *
         * Otherwise-unsafe constructs such as arbitrary pointers to data
         * outside the packet may be followed.
         * 后面可能跟着不安全的构造，例如指向包外数据的任意指针。
         **
        #define AV_PKT_FLAG_TRUSTED   0x0008
        **
         * Flag is used to indicate packets that contain frames that can
         * be discarded by the decoder.  I.e. Non-reference frames.
         * 标志用来指示包含可被解码器丢弃的帧的包。即非引用帧。
         **
        #define AV_PKT_FLAG_DISPOSABLE 0x0010


**********************************************************************/
//---------------------------------------------------------------------
//  包含头文件
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef __cplusplus
extern "C"
{
#endif  //  __cplusplus
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#ifdef __cplusplus
}
#endif  //  __cplusplus

//---------------------------------------------------------------------
//  相关宏定义
#define DEBUG_LOG                     0     //  是否开启打印Log

//---------------------------------------------------------------------
//  相关类型定义

//  输入类型定义
typedef enum
{
    EInputType_None = 0,       //  正常输入,可以为文件名,也可以为开关选项
    EInputType_OutputPath,     //  当为输出目录
}EInputType;

//  FFmpeg上下文数据结构
typedef struct
{
    //  相关控制信息
    AVFormatContext*    p_fmt_ctx;
    AVCodecContext*     p_codec_ctx; 
    AVCodecParameters*  p_codec_par;
    AVCodec*            p_codec;
    int                 buf_size;
    int                 v_idx;             //  视频流ID
    int                 a_idx;             //  音频流ID
    AVStream*           video_stream;      //  视频流
    AVStream*           audio_stream;      //  音频流

    //  要导出H264的一些必要信息
    unsigned char* sps_dat;
    unsigned char* pps_dat;
    int sps_len;
    int pps_len;

    //  一些标志
    bool avcodec_open_already;             //  解码器的打开标志

    //  视频信息
    float               FrameRate;         //  帧率
    int                 Width;             //  宽度
    int                 Height;            //  高度
    unsigned long       TotalFrame;        //  该视频总共帧数量
}SFFmpegContext;

//---------------------------------------------------------------------
//  相关变量

//  当前输入类型
EInputType CurrentInputType = EInputType_None;   //  默认为正常输入

//  解码器上下文
SFFmpegContext ffmpeg_context;

//  输出文件相关
std::vector<std::string> InputFileVec;            //  输入的文件容器 

//  开始代码
unsigned char startcode[4]={0x00, 0x00, 0x00, 0x01};

std::string OutputPath = "";      //  输出的目录(当为空的时候,输出的原输入目录)

//---------------------------------------------------------------------
//  其他封装函数

//  得到当前字符串中有多少个指定的符号
int GetStringCountChar(std::string in_str, char ch)
{
    int re = 0;
    int len = in_str.size();
    int i=0;
    for(i=0;i<len;i++)
    {
        if(in_str.at(i) == ch)
        {
            re++;
        }
    }
    return re;
}

//  从一个字符串中删除回车和换行
std::string DeleteNR(std::string in_str)
{
    int len = in_str.size();
    int i=0;
    for(i=0;i<len;i++)
    {
        if((in_str.at(i) == '\r') || (in_str.at(i) == '\n'))
        {
            in_str.erase(in_str.begin() + i);
            i--;
            len--;
        }
    }
    return in_str;
}

//  从路径中获取文件名，含扩展名
std::string GetFileNameExFromPath(std::string in_str)
{
    //  定义返回字符串
    std::string re_str;

    //  遍历每个字符
    int len = in_str.size();
    int i = 0;
    for(i=0;i<len;i++)
    {
        //  当不为路径分割符号，插入输出字符串
        if((in_str.at(len-1-i) != '\\')&&(in_str.at(len-1-i) != '/'))
        {
            re_str.insert(re_str.begin(), in_str.at(len-1-i));
        }
        //  当为路径分割，直接跳出
        else
        {
            break;
        }
    }

    //  返回字符串
    return re_str;
}

//  从文件名中删除文件中的扩展名，输入必须仅仅是带扩展名的文件名，不能是带路径的
std::string GetFileNameNoExFormFileName(std::string in_str)
{
    //  定义返回字符串
    std::string re_str;

    //  完全赋值给返回
    re_str = in_str;

    //  统计其中含有多少点
    int dot_cnt = 0;
    int len = in_str.size();
    int i = 0;
    for(i=0;i<len;i++)
    {
        //  当为点的时候
        if(in_str.at(len-1-i) == '.')
        {
            dot_cnt++;
        }
    }

    //  只要里面含有点
    if(dot_cnt >= 1)
    {
        //  遍历每个字符，从后面网前执行删除
        for(i=0;i<len;i++)
        {
            //  当为点
            if(re_str.at(len-1-i) == '.')
            {
                //  删除
                re_str.erase(re_str.end()-1);
                break;
            }
            //  不为点
            else
            {
                re_str.erase(re_str.end()-1);
            }
        }
    }

    //  返回字符串
    return re_str;
}

//  从完整路径或文件名中提取纯文件名部分，不含扩展名
std::string GetOnlyFileNameNoEx(std::string in_str)
{
    return GetFileNameNoExFormFileName(GetFileNameExFromPath(in_str));
}

//  从完整路径含文件名中提取出纯路径部分
std::string GetOnlyFilePath(std::string in_str)
{
    //  定义返回字符串
    std::string re_str;

    //  完全赋值给返回
    re_str = in_str;

    //  统计其中含有多少个路径分割符号
    int ch_cnt = 0;
    int len = in_str.size();
    int i = 0;
    for(i=0;i<len;i++)
    {
        //  当为路径分割符号的时候
        if((in_str.at(len-1-i) == '\\') || (in_str.at(len-1-i) == '/'))
        {
            ch_cnt++;
        }
    }

    //  只要里面含有路径分割符号
    if(ch_cnt >= 1)
    {
        //  遍历每个字符，从后面网前执行删除
        for(i=0;i<len;i++)
        {
            //  当为分割符号
            if((in_str.at(len-1-i) == '\\') || (in_str.at(len-1-i) == '/'))
            {
                //  删除
                re_str.erase(re_str.end()-1);
                break;
            }
            //  不为分割符号
            else
            {
                re_str.erase(re_str.end()-1);
            }
        }
    }
    //  当没有分隔符的时候，返回空
    else
    {
        re_str.clear();
    }

    //  返回字符串
    return re_str;
}

//---------------------------------------------------------------------
//  打开一个视频文件
int FFMpeg_OpenVideo(std::string filename)
{
    //  定义返回值
    int re = -1;

    //  打开视频文件
    re = avformat_open_input(&ffmpeg_context.p_fmt_ctx,
                             filename.c_str(),
                             NULL, NULL
                            );
    if(re != 0)
    {
        printf("ERROR:avformat_open_input()\r\n");
        ffmpeg_context.p_fmt_ctx = 0;
        return -1;
    }

    //  搜索流信息
    re = avformat_find_stream_info(ffmpeg_context.p_fmt_ctx,
                                   NULL
                                  );
    if(re != 0)
    {
        if(ffmpeg_context.p_fmt_ctx != 0)
        {
            avformat_close_input(&ffmpeg_context.p_fmt_ctx);
            ffmpeg_context.p_fmt_ctx = 0;
        }
        printf("ERROR:avformat_find_stream_info()\r\n");
        return -2;
    }

    //  打印流信息
#if DEBUG_LOG
    av_dump_format(ffmpeg_context.p_fmt_ctx, 0, filename.c_str(), 0);
#endif  //  FFMPEG_DEBUG_LOG

    //  查找第一个视频流 和 音频流
    ffmpeg_context.v_idx = -1;
    ffmpeg_context.a_idx = -1;
    int i=0;
    for (i=0; i<ffmpeg_context.p_fmt_ctx->nb_streams; i++)
    {
        //  当为视频流
        if (ffmpeg_context.p_fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            ffmpeg_context.v_idx = i;
            ffmpeg_context.TotalFrame = ffmpeg_context.p_fmt_ctx->streams[i]->nb_frames;
            printf("Find a video stream, index %d\r\n", ffmpeg_context.v_idx);
            printf("Total Frame = %ld\r\n", ffmpeg_context.TotalFrame);
            ffmpeg_context.FrameRate = 
                (ffmpeg_context.p_fmt_ctx->streams[i]->avg_frame_rate.num * 1.0f)/
                    ffmpeg_context.p_fmt_ctx->streams[i]->avg_frame_rate.den;
            break;
        }
        //  当为音频流
        if(ffmpeg_context.p_fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            ffmpeg_context.a_idx = i;
            printf("Find a Audio stream, index %d\r\n", ffmpeg_context.a_idx);
        }
    }
    if (ffmpeg_context.v_idx == -1)
    {
        printf("ERROR:Cann't find a video stream\r\n");
        if(ffmpeg_context.p_fmt_ctx != 0)
        {
            avformat_close_input(&ffmpeg_context.p_fmt_ctx);
            ffmpeg_context.p_fmt_ctx = 0;
        }
        return -3;
    }
    ffmpeg_context.video_stream = 
        ffmpeg_context.p_fmt_ctx->streams[ffmpeg_context.v_idx];
    printf("frame_rate = %f fps\r\n", ffmpeg_context.FrameRate);
    if(ffmpeg_context.a_idx == -1)
    {
#if FFMPEG_HINT_LOG
        printf("WARNNING:Cann't find a audio stream\r\n");
#endif  //  FFMPEG_HINT_LOG
    }
    else
    {
        ffmpeg_context.audio_stream = 
            ffmpeg_context.p_fmt_ctx->streams[ffmpeg_context.a_idx];
    }

    //  为视频流构造解码器
    //  获取解码器参数
    ffmpeg_context.p_codec_par = 
        ffmpeg_context.p_fmt_ctx->streams[ffmpeg_context.v_idx]->codecpar;

    //  获取解码器
    //  限制解码器
    ffmpeg_context.p_codec = avcodec_find_decoder_by_name("h264");
    if(ffmpeg_context.p_codec == NULL)
    {
        if(ffmpeg_context.p_fmt_ctx != 0)
        {
            avformat_close_input(&ffmpeg_context.p_fmt_ctx);
            ffmpeg_context.p_fmt_ctx = 0;
        }
        printf("ERROR:avcodec_find_decoder()\r\n");
        return -4;
    }

    //  构造解码器
    ffmpeg_context.p_codec_ctx = avcodec_alloc_context3(ffmpeg_context.p_codec);
    if(ffmpeg_context.p_codec_ctx == NULL)
    {
        if(ffmpeg_context.p_fmt_ctx != 0)
        {
            avformat_close_input(&ffmpeg_context.p_fmt_ctx);
            ffmpeg_context.p_fmt_ctx = 0;
        }
        printf("ERROR:avcodec_alloc_context3()\r\n");
        return -5;
    }

    //  解码器参数初始化
    re = avcodec_parameters_to_context(ffmpeg_context.p_codec_ctx, 
                                       ffmpeg_context.p_codec_par
                                      );
    if(re < 0)
    {
        if(ffmpeg_context.p_codec_ctx != 0)
        {
            avcodec_free_context(&ffmpeg_context.p_codec_ctx);
            ffmpeg_context.p_codec_ctx = 0;
        }
        if(ffmpeg_context.p_fmt_ctx != 0)
        {
            avformat_close_input(&ffmpeg_context.p_fmt_ctx);
            ffmpeg_context.p_fmt_ctx = 0;
        }
        printf("ERROR:avcodec_parameters_to_context()\r\n");
        return -6;
    }

    //  打开解码器
    re = avcodec_open2(ffmpeg_context.p_codec_ctx, ffmpeg_context.p_codec, NULL);
    if(re < 0)
    {
        if(ffmpeg_context.p_codec_ctx != 0)
        {
            avcodec_free_context(&ffmpeg_context.p_codec_ctx);
            ffmpeg_context.p_codec_ctx = 0;
        }
        if(ffmpeg_context.p_fmt_ctx != 0)
        {
            avformat_close_input(&ffmpeg_context.p_fmt_ctx);
            ffmpeg_context.p_fmt_ctx = 0;
        }
        printf("ERROR:avcodec_open2()\r\n");
        return -7;
    }

    //  解码器打开成功
    ffmpeg_context.avcodec_open_already = true;

    //  提示找到解码器的名字
    printf("Find Codec Name:%s\r\n", ffmpeg_context.p_codec->name);

    //  当解码器名字不匹配
    std::string decodec_name = ffmpeg_context.p_codec->name;
    if(decodec_name != "h264")
    {
        //  依次释放资源
        if(ffmpeg_context.avcodec_open_already)
        {
            avcodec_close(ffmpeg_context.p_codec_ctx);
            ffmpeg_context.avcodec_open_already = false;
        }
        if(ffmpeg_context.p_codec_ctx != 0)
        {
            avcodec_free_context(&ffmpeg_context.p_codec_ctx);
            ffmpeg_context.p_codec_ctx = 0;
        }
        if(ffmpeg_context.p_fmt_ctx != 0)
        {
            avformat_close_input(&ffmpeg_context.p_fmt_ctx);
            ffmpeg_context.p_fmt_ctx = 0;
        }
        return -8;
    }

    //  配置宽度、高度
    ffmpeg_context.Width = ffmpeg_context.p_codec_ctx->width;
    ffmpeg_context.Height = ffmpeg_context.p_codec_ctx->height;
    printf("width=%d, height=%d\r\n", ffmpeg_context.Width, ffmpeg_context.Height);

    //  获取SPS相关
    //  计算SPS的长度
    ffmpeg_context.sps_len = ffmpeg_context.video_stream->codecpar->extradata[6] * 0xFF +
                             ffmpeg_context.video_stream->codecpar->extradata[7];
#if DEBUG_LOG
    printf("SPS len = %d(bytes)\r\n", ffmpeg_context.sps_len);
#endif  //  DEBUG_LOG

    //  复制SPS数据
    ffmpeg_context.sps_dat = new unsigned char[ffmpeg_context.sps_len];
    memcpy(ffmpeg_context.sps_dat,
           ffmpeg_context.video_stream->codecpar->extradata + 8,
           ffmpeg_context.sps_len
          );

    //  获取PPS相关
    //  计算PPS长度
    ffmpeg_context.pps_len = ffmpeg_context.video_stream->codecpar->extradata[8 + ffmpeg_context.sps_len + 1] * 0xFF +
                             ffmpeg_context.video_stream->codecpar->extradata[8 + ffmpeg_context.sps_len + 2];
#if DEBUG_LOG
    printf("PPS len = %d(bytes)\r\n", ffmpeg_context.pps_len);
#endif  //  DEBUG_LOG

    //  获取PPS数据
    ffmpeg_context.pps_dat = new unsigned char[ffmpeg_context.pps_len];
    memcpy(ffmpeg_context.pps_dat,
           ffmpeg_context.video_stream->codecpar->extradata + 8 + 2 + 1 + ffmpeg_context.sps_len,
           ffmpeg_context.pps_len
          );

    //  操作成功
    return 0;
}

//  关闭当前已经打开的视频文件
void FFMpeg_CloseVideo(void)
{
    //  依次释放资源
    if(ffmpeg_context.sps_dat != 0)
    {
        delete [] ffmpeg_context.sps_dat;
        ffmpeg_context.sps_dat = 0;
        ffmpeg_context.sps_len = 0;
    }
    if(ffmpeg_context.pps_dat != 0)
    {
        delete [] ffmpeg_context.pps_dat;
        ffmpeg_context.pps_dat = 0;
        ffmpeg_context.pps_len = 0;
    }
    if(ffmpeg_context.avcodec_open_already)
    {
        avcodec_close(ffmpeg_context.p_codec_ctx);
        ffmpeg_context.avcodec_open_already = false;
    }
    if(ffmpeg_context.p_codec_ctx != 0)
    {
        avcodec_free_context(&ffmpeg_context.p_codec_ctx);
        ffmpeg_context.p_codec_ctx = 0;
    }
    if(ffmpeg_context.p_fmt_ctx != 0)
    {
        avformat_close_input(&ffmpeg_context.p_fmt_ctx);
        ffmpeg_context.p_fmt_ctx = 0;
    }
}

//---------------------------------------------------------------------
//  H264解码相关函数

//  检查是否包含SEI区头部
//  参数 pdat 为数据首地址
//  参数 len 为数据有效长度
//  包含SEI信息头返回true, 否则返回false
bool H264_CheckSEI_Inside(unsigned char* pdat, int len)
{
    //  检查长度
    if(len < 6) return false;

    //  检查指针
    if(pdat == 0) return false;

    //  当为SEI
    if((pdat[4] == 0x06)  &&    //  为SEI数据头
       (pdat[5] == 0x05)        //  暂时只支持user_data_unregistered()语法
      )
    {
        return true;
    }
    else
    {
        return false;
    }
}

//  获取SEI头部长度,同时获取UUID+用户区长度
//  参数 pdat 为数据首地址
//  参数 len 为数据有效长度
//  返回的长度值 包含SEI头部
//  即 NAL头部+代码类型+长度字节 的总长度
//  失败返回小于0
int H264_SEI_GetHeadLen(unsigned char* pdat, int len, int& uuid_content_len)
{
    //  当头部检查通过
    if(!H264_CheckSEI_Inside(pdat, len)) return -1;

    //  定义长度
    int total_len = 0;
    int byte_cnt = 0;
    while(1)
    {
        //  获取当前长度字节
        int tmp_len = pdat[4 + 1 + 1 + byte_cnt] & 0x0FF;

        //  统计
        byte_cnt++;
        total_len += tmp_len;

        //  当到达结尾
        if(tmp_len != 0x0FF)
        {
            break;
        }
    }

    //  设置UUID+用户区长度
    uuid_content_len = total_len;

    //  返回头部总长度
    return 4 + 1 + 1 + byte_cnt;
}

//  获取SEI用户定义区长度
//  参数 pdat 为数据首地址
//  参数 len 为数据有效长度
//  返回的长度值 包含SEI的用户数据区(通常为ASCII文本)
//  即 自定义数据区 的总长度
//  失败返回小于0
int H264_SEI_GetContentLen(unsigned char* pdat, int len)
{
    //  获取头部长度 和 UUID+用户区总长度
    int uuid_content_len = 0;
    int re = H264_SEI_GetHeadLen(pdat, len, uuid_content_len);

    //  检查
    if((re >= (4 + 1 + 1 + 1)) &&
       (uuid_content_len > 16)
      )
    {
        return uuid_content_len - 16;
    }
    //  错误
    else
    {
        return -1;
    }
}

//  获取SEI总长度
//  参数 pdat 为数据首地址
//  参数 len 为数据有效长度
//  返回的长度值 包含整个SEI数据段
//  即 NAL头部+代码类型+长度字节+UUID+自定义数据区+结尾字节 的总长度
//  失败返回小于0
int H264_SEI_GetTotalDataLen_SEI(unsigned char* pdat, int len)
{
    //  当头部检查通过
    if(!H264_CheckSEI_Inside(pdat, len)) return -1;

    //  获取SEI头部的长度 和 UUID+用户区总长度
    int uuid_content_len = 0;
    int head_len = H264_SEI_GetHeadLen(pdat, len, uuid_content_len);

    //  检查获取结果
    if((head_len >= (4 + 1 + 1 + 1)) &&
       (uuid_content_len > 16)
      )
    {
    }
    //  错误
    else
    {
        return -1;
    }

    //  计算总长度
    return head_len + uuid_content_len + 1;
}


//  从SEI区域中提取UUID(固定16个字节)(SEI payload UUID)
//  参数 pdat 为数据首地址
//  参数 len 为数据有效长度
//  操作成功返回有16个字节长度的容器,失败返回空容器
std::vector<unsigned char> H264_SEI_GetUUID(unsigned char* pdat, int len)
{
    //  定义返回变量
    std::vector<unsigned char> re_vec;

    //  计算偏移
    int uuid_content_len = 0;
    int uuid_offset = H264_SEI_GetHeadLen(pdat, len, uuid_content_len);

    //  检查
    if(uuid_offset <= 0) return re_vec;

    //  复制信息
    int i=0;
    for(i=0;i<16;i++)
    {
        re_vec.insert(re_vec.end(), pdat[uuid_offset + i]);
    }

    //  返回
    return re_vec;
}

//  从SEI区域中提取用户自定义数据(SEI payload content)
//  参数 pdat 为数据首地址
//  参数 len 为数据有效长度
//  操作成功返回非0字节长度的容器,失败返回空容器
std::vector<unsigned char> H264_SEI_GetContent(unsigned char* pdat, int len)
{
    //  定义返回变量
    std::vector<unsigned char> re_vec;

    //  计算偏移
    int uuid_content_len = 0;
    int uuid_offset = H264_SEI_GetHeadLen(pdat, len, uuid_content_len);

    //  检查
    if(uuid_offset <= 0) return re_vec;
    if(uuid_content_len <= 16) return re_vec;

    //  计算偏移
    int content_offset = uuid_offset + 16;

    //  计算长度
    int content_len = uuid_content_len - 16;

    //  复制信息
    int i=0;
    for(i=0;i<content_len;i++)
    {
        re_vec.insert(re_vec.end(), pdat[content_offset + i]);
    }

    //  返回
    return re_vec;
}

//  以ASCII形式dump出vector中的信息
void ASCII_DumpVector(std::vector<unsigned char> in_vec)
{
    //  检查长度
    int len = in_vec.size();
    if(len <= 0)
    {
        printf("[Error] [Vector is NULL!!]\r\n");
        return;
    }

    //  申请内存
    unsigned char* pbuf = new unsigned char[len + 1];
    if(pbuf == 0)
    {
        printf("[Error] new Error!!\r\n");
        return;
    }
    memset(pbuf, 0, len + 1);

    //  复制数据
    int i=0;
    for(i=0;i<len;i++)
    {
        pbuf[i] = in_vec.at(i);
    }

    //  打印
    printf("%s\r\n", pbuf);

    //  结束
    delete [] pbuf;
}

//  以十六进制dump出UUID值
void HexUUID_DumpVector(std::vector<unsigned char> in_vec)
{
    //  检查长度
    int len = in_vec.size();
    if(len != 16)
    {
        printf("[Error] UUID len Error!!\r\n");
        return;
    }

    //  申请内存
    unsigned char* pbuf = new unsigned char[len];
    if(pbuf == 0)
    {
        printf("[Error] new Error!!\r\n");
        return;
    }
    memset(pbuf, 0, len);

    //  打印UUID的各个部分
    int i=0;
    for(i=0;i<4;i++)
    {
        printf("%02X" ,in_vec.at(i));
    }
    printf("-");
    for(i=0;i<2;i++)
    {
        printf("%02X" ,in_vec.at(4 + i));
    }
    printf("-");
    for(i=0;i<2;i++)
    {
        printf("%02X" ,in_vec.at(4 + 2 + i));
    }
    printf("-");
    for(i=0;i<2;i++)
    {
        printf("%02X" ,in_vec.at(4 + 2 + 2 + i));
    }
    printf("-");
    for(i=0;i<6;i++)
    {
        printf("%02X" ,in_vec.at(4 + 2 + 2 + 2 + i));
    }
    printf("\r\n");

    //  结束
    delete [] pbuf;
}

//---------------------------------------------------------------------
//  主函数
int main(int argc, char** argv)
{
    //  初始化参数
    ffmpeg_context.p_fmt_ctx = NULL;
    ffmpeg_context.p_codec_ctx = NULL;
    ffmpeg_context.p_codec_par = NULL;
    ffmpeg_context.p_codec = NULL;
    ffmpeg_context.buf_size = 0;
    ffmpeg_context.v_idx = -1;
    ffmpeg_context.a_idx = -1;
    ffmpeg_context.video_stream = 0;
    ffmpeg_context.audio_stream = 0;

    ffmpeg_context.sps_dat = 0;
    ffmpeg_context.sps_len = 0;
    ffmpeg_context.pps_dat = 0;
    ffmpeg_context.pps_len = 0;

    ffmpeg_context.avcodec_open_already = false;

    ffmpeg_context.FrameRate = 0.0f;
    ffmpeg_context.Width = 0;
    ffmpeg_context.Height = 0;
    ffmpeg_context.TotalFrame = 0UL;

    //  检查输入参数
    if(argc < 2)
    {
        printf("Input Arg Number Error!!\r\n");
        return -1;
    }

    //  遍历全部输入参数
    int i=0;
    for(i=1;i<argc;i++)
    {
        //  当为正常输入类型
        if(CurrentInputType == EInputType_None)
        {
            //  当为表示输出路径设置的开关
            if(strcmp("-o", argv[i]) == 0)
            {
                CurrentInputType = EInputType_OutputPath;
            }
            //  其他情况
            else
            {
                //  将文件名含完整路径部分插入输入文件列表中
                InputFileVec.insert(InputFileVec.end(), argv[i]);
            }
        }
        //  当为输出路径
        else if(CurrentInputType == EInputType_OutputPath)
        {
            //  设置输出路径
            OutputPath = argv[i];

            //  恢复开关到默认
            CurrentInputType = EInputType_None;
        }
        //  错误类型
        else
        {
            printf("Error Input Type!!\r\n");
            return -2;
        }
    }

    //  打印识别结果
    int input_file_total = InputFileVec.size();
#if DEBUG_LOG
    printf("Total Input File Count is %d\r\n", input_file_total);
    printf("Input File List:\r\n");
    for(i=0;i<input_file_total;i++)
    {
        printf("    %s\r\n", InputFileVec.at(i).c_str());
    }
#endif  //  DEBUG_LOG

    //  定义返回值
    int re = 0;

    //  循环操作视频文件
    for(i=0;i<input_file_total;i++)
    {
        //  打印当前正在处理的视频文件名字(源文件名字)
        printf("-----Current Video Conv File:%s\r\n", InputFileVec.at(i).c_str());

        //  提取输入视频文件的路径
        std::string input_video_path = GetOnlyFilePath(InputFileVec.at(i));

        //  提取纯文件名部分(不含扩展名)
        std::string input_video_only_name = GetOnlyFileNameNoEx(InputFileVec.at(i));

        //  打开视频文件
        re = FFMpeg_OpenVideo(InputFileVec.at(i));

        //  当打开成功
        if(re == 0)
        {
            //  写入视频信息文件
            std::string output_vinf_name;
            //  当输出目录为空目录
            if(OutputPath == "")
            {
                //  使用输入源文件路径
                if(input_video_path == "") output_vinf_name = input_video_only_name + ".vinf";
                else                       output_vinf_name = input_video_path + "/" + input_video_only_name + ".vinf";
            }
            //  输出目录不为空
            else
            {
                //  使用设定路径
                output_vinf_name = OutputPath + "/" + input_video_only_name + ".vinf";
            }
        #if DEBUG_LOG
            printf("Output Video Info File Name:%s\r\n", output_vinf_name.c_str());
        #endif
            FILE* pfile_outvinf = fopen(output_vinf_name.c_str(), "wb");

            //  写入信息
            fprintf(pfile_outvinf, "%d %d %0.1f %ld\r\n",
                    ffmpeg_context.Width,
                    ffmpeg_context.Height,
                    ffmpeg_context.FrameRate,
                    ffmpeg_context.TotalFrame
                   );

            //  累计本帧字节数
            int frame_byte_cnt = 0;

            //  创建只写文件(输出纯H264的视频流文件)
            std::string output_h264_name;
            //  当输出目录为空目录
            if(OutputPath == "")
            {
                //  使用输入源文件路径
                if(input_video_path == "") output_h264_name = input_video_only_name + ".h264";
                else                       output_h264_name = input_video_path + "/" + input_video_only_name + ".h264";
            }
            //  输出目录不为空
            else
            {
                //  使用设定路径
                output_h264_name = OutputPath + "/" + input_video_only_name + ".h264";
            }
        #if DEBUG_LOG
            printf("Output Video H264 File Name:%s\r\n", output_h264_name.c_str());
        #endif  //  DEBUG_LOG
            FILE* pfile_outh264 = fopen(output_h264_name.c_str(), "wb");

            //  开始写入一些关键头部信息
            //------------------------------------------------------------------
            //  写入SPS
            //  写入每个部分之前都先写入开始代码
        #if DEBUG_LOG
            printf("Begin Write SPS...\r\n");
        #endif  //  DEBUG_LOG
            re = fwrite(startcode, 1, sizeof(startcode), pfile_outh264);
            if(re != sizeof(startcode))
            {
                printf("[Error] SPS StartCode Write Error!! in_byte=%ld, re=%d\r\n", sizeof(startcode), re);
                fclose(pfile_outh264);
                FFMpeg_CloseVideo();
                return -4;
            }
            frame_byte_cnt += sizeof(startcode);

            //  写入SPS数据区
            re = fwrite(ffmpeg_context.sps_dat, 1, ffmpeg_context.sps_len, pfile_outh264);
            if(re != ffmpeg_context.sps_len)
            {
                printf("[Error] SPS Data Write Error!! in_byte=%d, re=%d\r\n", ffmpeg_context.sps_len, re);
                fclose(pfile_outh264);
                FFMpeg_CloseVideo();
                return -5;
            }
            frame_byte_cnt += ffmpeg_context.sps_len;
            
            //------------------------------------------------------------------
            //  写入PPS
            //  写入每个部分之前都先写入开始代码
        #if DEBUG_LOG
            printf("Begin Write PPS...\r\n");
        #endif  //  DEBUG_LOG
            re = fwrite(startcode, 1, sizeof(startcode), pfile_outh264);
            if(re != sizeof(startcode))
            {
                printf("[Error] PPS StartCode Write Error!! in_byte=%ld, re=%d\r\n", sizeof(startcode), re);
                fclose(pfile_outh264);
                FFMpeg_CloseVideo();
                return -6;
            }
            frame_byte_cnt += sizeof(startcode);

            //  写入PPS数据区
            re = fwrite(ffmpeg_context.pps_dat, 1, ffmpeg_context.pps_len, pfile_outh264);
            if(re != ffmpeg_context.pps_len)
            {
                printf("[Error] PPS Data Write Error!! in_byte=%d, re=%d\r\n", ffmpeg_context.pps_len, re);
                fclose(pfile_outh264);
                FFMpeg_CloseVideo();
                return -7;
            }
            frame_byte_cnt += ffmpeg_context.pps_len;

            //  定义包
            AVPacket *pkt = 0;

            // 分配原始文件流packet的缓存
            pkt = av_packet_alloc();

            //  定义帧计数器
            unsigned long frame_cnt = 0UL;

            //------------------------------------------------------------------
            //  循环写入每一帧的码流
            //  开始循环抓取每一帧
        #if DEBUG_LOG
            printf("Begin while(1)...\r\n");
        #endif  //  DEBUG_LOG
            while(1)
            {
                //  检索视频包
                //  从视频文件中获取一个包
            #if DEBUG_LOG
                printf("av_read_frame...\r\n");
            #endif  //  DEBUG_LOG
                while(av_read_frame(ffmpeg_context.p_fmt_ctx, pkt) >= 0)
                {
                    //  当读取到一帧视频的时候，则跳出
                    if(pkt->stream_index == ffmpeg_context.v_idx)
                    {
                        //  找到了
                    #if 1
                        //  当为数据被破坏的包
                        if((pkt->flags & AV_PKT_FLAG_CORRUPT) != 0)
                        {
                            av_packet_unref(pkt);   //  丢弃
                        }
                        //  不安全的结构的包
                        else if((pkt->flags & AV_PKT_FLAG_DISCARD) != 0)
                        {
                            av_packet_unref(pkt);   //  丢弃
                        }
                        //  可能被解码器丢弃的包
                        else if((pkt->flags & AV_PKT_FLAG_DISPOSABLE) != 0)
                        {
                            //av_packet_unref(pkt);   //  丢弃
                            break;
                        }
                        //  正常的数据包
                        else
                        {
                            break;
                        }
                    #else
                        //  当为关键帧
                        if((pkt->flags & AV_PKT_FLAG_KEY) != 0)
                        {
                            break;
                        }
                        //  不为关键帧
                        else
                        {
                            av_packet_unref(pkt);   //  丢弃
                        }
                    #endif
                    }
                    else
                    {
                        av_packet_unref(pkt);
                    }
                }

            #if DEBUG_LOG
                printf("memcpy startcode...\r\n");
                printf("pkt->size = %d\r\n", pkt->size);
            #endif  //  DEBUG_LOG

                //  检查包长度
                if(pkt->size < sizeof(startcode))
                {
                    //ffmpeg_context.TotalFrame--;      //  少一帧
                    av_packet_unref(pkt);    //  跳出
                    break;
                }

                //  替换本数据流的开始代码
                memcpy(pkt->data, startcode, sizeof(startcode));

                //  检查该帧中是否含有SEI信息
            #if DEBUG_LOG
                printf("check sei...\r\n");
            #endif  //  DEBUG_LOG
                if(H264_CheckSEI_Inside(pkt->data, pkt->size))
                {
                    //  打印SEI的UUID
                    printf("H264 Video SEI Payload UUID:");
                    HexUUID_DumpVector(H264_SEI_GetUUID(pkt->data, pkt->size));

                    //  打印SEI的用户信息
                #if DEBUG_LOG
                    printf("H264 Video SEI Payload Content:");
                    ASCII_DumpVector(H264_SEI_GetContent(pkt->data, pkt->size));
                #endif  //  DEBUG_LOG

                    //  获得整个SEI段的总长度
                    int total_sei_len = H264_SEI_GetTotalDataLen_SEI(pkt->data, pkt->size);

                    //  当合法
                    if(total_sei_len > 0)
                    {
                        //  修改SEI段后面的关键帧的StartCode
                        memcpy(pkt->data + total_sei_len, startcode, sizeof(startcode));
                    }
                }

                //  保存h264码流
            #if DEBUG_LOG
                printf("fwrite...\r\n");
            #endif  //  DEBUG_LOG
                re = fwrite(pkt->data, 1, pkt->size, pfile_outh264);

                //  检查文件是否写入成功
                //  当写入失败
                if(re != pkt->size)
                {
                    printf("[Error] H264 Output Video File Write Error!! in_byte=%d, re=%d\r\n", pkt->size, re);
                    av_packet_unref(pkt);
                    AVPacket *ppkt[1];
                    ppkt[0] = pkt;
                    av_packet_free(ppkt);
                    fclose(pfile_outh264);
                    FFMpeg_CloseVideo();
                    return -3;
                }
                frame_byte_cnt += pkt->size;

                //  将本次写入的尺寸统计到信息文件中
                fprintf(pfile_outvinf, "%d\r\n", frame_byte_cnt);
                frame_byte_cnt = 0;

                //  统计一帧
            #if DEBUG_LOG
                printf("frame = %ld...\r\n", frame_cnt);
            #endif  //  DEBUG_LOG
                frame_cnt++;

                //  当达到视频末尾
                if(frame_cnt >= ffmpeg_context.TotalFrame)
                {
                    av_packet_unref(pkt);
                    break;
                }
            }

            //  释放包
            AVPacket *ppkt[1];
            ppkt[0] = pkt;
            av_packet_free(ppkt);

            //  关闭输出文件
            fclose(pfile_outh264);

            //  视频信息文件写入完成
            fclose(pfile_outvinf);

        }
        //  打开失败
        else
        {
            printf("[Error] Open Video File Error!! Return Code=%d\r\n", re);
            FFMpeg_CloseVideo();
            return -2;
        }

        //  释放相关资源
        FFMpeg_CloseVideo();
    }


    //  程序正常结束
    return 0;
}

