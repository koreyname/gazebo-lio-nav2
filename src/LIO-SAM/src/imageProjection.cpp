/****************************************************************************
************step1-----imageProjection（点云预处理）****************************

功能：
+ imageProjection的主要功能是订阅原始点云数据和imu数据，根据高频的imu信息对点云成像时雷达的位移和旋转
  造成的畸变进行校正；同时，在发布去畸变点云的时候加入IMU输出的角度和IMU里程计（imuPreintegration）的
  角度和位姿作为该帧的初始位姿，作为图优化的初始估计；并且，要对点云的Range进行计算，同时记录每个点的行列，
  以便在特征提取中被使用。

订阅：
1. IMU原始数据；
2. Lidar原始数据；
3. IMU里程计（来自imuPreintegration），IMU里程计融合了低频激光里程计数据（更准确）
   和高频的IMU数据（噪声较大），比直接用原始IMU数据做积分更准确。

发布：
   去畸变后的点云及附属信息，包括：
   原始点云、去畸变点云、该帧点云的初始旋转角（来自IMU原始roll、pitch、yaw）、
   该帧点云的初始位姿（来自IMU里程计）。

流程：
1. 接收到一帧点云，从IMU原始数据队列找到该帧点云时间戳对应的数据，
   将IMU的roll、pitch、yaw塞进准备发布的该帧点云信息；
2. 提取该帧点云的起止时间戳（激光雷达点云的每个点都有相对于该帧起始时间的时间间隔），
   对起止时间内的IMU数据进行角度积分，得到该帧点云每个时刻对应的旋转。
  （注意，这里算法使用的是简单的角度累加，实际上是积分的近似，但是在很短的时间内，
   10Hz雷达对应100ms的扫描时间，近似的累加代替角度积分计算更快可以保证实时性）；
3. 遍历该帧点云每个点，旋转到起始点坐标系，从IMU里程计提取该帧点云对应的位姿（包括位置和旋转），
   塞进准备发布的该帧点云信息；
4. 发布该帧点云信息。

备注：
1. LIO-SAM要求使用九轴IMU（有roll、pitch、yaw输出），但是这里却没有直接使用输出的roll、pitch、yaw。
后续应该可以把整个框架改成只需要六轴IMU
2. 去畸变过程只应用了旋转，没有应用平移，代码中原始注释说在低速情况下不需要。
*******************************************************************************/
#include "lio_sam_op/utility.hpp"
#include "lio_sam_op/msg/cloud_info.hpp"

/**
 *  Velodyne激光雷达的数据类型
 * 这里使用16bytes对齐，是因为SIMD(Single Instruction Multiple Data)架构需要使用128bit对齐，
 * 跟现代CPU架构和指令集有关*/
struct VelodynePointXYZIRT
{
    PCL_ADD_POINT4D
    PCL_ADD_INTENSITY;
    uint16_t ring; // 表示该点所在的激光雷达环的编号
    float time;
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW
} EIGEN_ALIGN16;

/**
 * pcl注册自定义点云类型：
 * https://pcl.readthedocs.io/projects/tutorials/en/latest/adding_custom_ptype.html
 */
POINT_CLOUD_REGISTER_POINT_STRUCT(VelodynePointXYZIRT,
                                  (float, x, x)(float, y, y)(float, z, z)(float, intensity, intensity)(uint16_t, ring, ring)(float, time, time))

/*
Ouster激光雷达的数据类型
*/
struct OusterPointXYZIRT
{
    PCL_ADD_POINT4D;
    float intensity;
    uint32_t t;
    uint16_t reflectivity;
    uint8_t ring;
    uint16_t ambient;
    uint32_t range;
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW
} EIGEN_ALIGN16;
POINT_CLOUD_REGISTER_POINT_STRUCT(OusterPointXYZIRT,
                                  (float, x, x)(float, y, y)(float, z, z)(float, intensity, intensity)(uint32_t, t, t)(uint16_t, reflectivity, reflectivity)(uint8_t, ring, ring)(uint16_t, ambient, ambient)(uint32_t, range, range))

// 默认使用Velodyne点类型
using PointXYZIRT = VelodynePointXYZIRT;

// IMU数据队列长度
const int queueLength = 2000;

class ImageProjection : public ParamServer
{
private:
    // IMU队列和IMU里程计队列线程锁
    std::mutex imuLock_;
    std::mutex odoLock_;

    // 订阅原始IMU数据
    rclcpp::Subscription<sensor_msgs::msg::Imu>::SharedPtr subImu_;
    rclcpp::CallbackGroup::SharedPtr callbackGroupImu_;
    std::deque<sensor_msgs::msg::Imu> imuQueue_;
    // 订阅原始雷达点云数据
    rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr subLaserCloud_;
    rclcpp::CallbackGroup::SharedPtr callbackGroupLidar_;
    std::deque<sensor_msgs::msg::PointCloud2> cloudQueue_;
    // 订阅IMU里程计数据，来自imuPreintegration
    rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr subImuOdom_;
    rclcpp::CallbackGroup::SharedPtr callbackGroupImuOdom_;
    std::deque<nav_msgs::msg::Odometry> imuOdomQueue_;

    // 发布去畸变后的点云信息。自定义消息lio_sam_op::msg::CloudInfo包括：
    // 原始点云、去畸变点云、该帧点云的初始旋转旋转角（来自IMU原始roll、pitch、yaw）、
    // 该帧点云的初始位姿（来自IMU里程计）
    rclcpp::Publisher<lio_sam_op::msg::CloudInfo>::SharedPtr pubLaserCloudInfo_;

    // 从点云队列中提取出当前点云帧做处理
    sensor_msgs::msg::PointCloud2 currentCloudMsg_;

    // 记录每一帧点云从起始到结束过程所有的IMU数据，imuRotX,Y,Z是对这一段时间内的角速度累加的结果
    double *imuTime_ = new double[queueLength];
    double *imuRotX_ = new double[queueLength];
    double *imuRotY_ = new double[queueLength];
    double *imuRotZ_ = new double[queueLength];

    // 记录每一帧点云起止过程中imuTime、imuRotXYZ的实际数据长度
    int imuPointerCur_;
    // 处理第一个点时，将该点的旋转取逆，记录到transStartInverse中，后续方便计算旋转的增量
    bool firstPointFlag_;
    // 处理第一个点时，将该点的旋转取逆，记录到transStartInverse中，后续方便计算旋转的增量
    Eigen::Affine3f transStartInverse_;

    pcl::PointCloud<PointXYZIRT>::Ptr laserCloudIn_;
    pcl::PointCloud<OusterPointXYZIRT>::Ptr tmpOusterCloudIn_;
    pcl::PointCloud<PointType>::Ptr fullCloud_;
    pcl::PointCloud<PointType>::Ptr extractedCloud_;

    // 当点云的time/t字段不可用，也就是点云中不包含每个点的时间戳，无法进行去畸变，直接返回原始点云
    int deskewFlag_;
    // 存储点云的range图像
    cv::Mat rangeMat_;

    // 是否有合适的IMU里程计数据
    bool odomDeskewFlag_;
    // 记录从IMU里程计出来的平移增量，用来做平移去畸变，实际中没有使用到
    float odomIncreX_;
    float odomIncreY_;
    float odomIncreZ_;

    // 发布的数据结构
    lio_sam_op::msg::CloudInfo cloudInfo_;
    // 当前雷达帧的起始时间
    double timeScanCur_;
    // 当前雷达帧的结束时间
    double timeScanEnd_;
    // 当前雷达帧的header
    std_msgs::msg::Header cloudHeader_;

public:
    ImageProjection(const rclcpp::NodeOptions &options)
        : ParamServer("lio_sam_imageProjection", options), deskewFlag_(0)
    {
        /*
        这里使用ROS2的callbackGroup，create_callback_group创建监听器，用于数据订阅。
        在ROS2节点中使用多线程,就涉及到回调组（CallbackGroup）了create_callback_group。
        CallbackGroupType包括MutuallyExclusive和Reentrant。
        MutuallyExclusive表示此组的回调函数是互斥的，不能在同一时间被执行。
        Reentrant表示回调函数是可重入的，允许同一时刻被多次执行。
        */
        callbackGroupLidar_ = create_callback_group(
            rclcpp::CallbackGroupType::MutuallyExclusive);
        callbackGroupImu_ = create_callback_group(
            rclcpp::CallbackGroupType::MutuallyExclusive);
        callbackGroupImuOdom_ = create_callback_group(
            rclcpp::CallbackGroupType::MutuallyExclusive);

        auto lidarOpt = rclcpp::SubscriptionOptions();
        lidarOpt.callback_group = callbackGroupLidar_;
        auto imuOpt = rclcpp::SubscriptionOptions();
        imuOpt.callback_group = callbackGroupImu_;
        auto imuOdomOpt = rclcpp::SubscriptionOptions();
        imuOdomOpt.callback_group = callbackGroupImuOdom_;

        subImu_ = create_subscription<sensor_msgs::msg::Imu>(
            imuTopic, qos_imu,
            std::bind(&ImageProjection::imuHandler, this, std::placeholders::_1),
            imuOpt);
        subImuOdom_ = create_subscription<nav_msgs::msg::Odometry>(
            imuOdomTopic, qos_imu,
            std::bind(&ImageProjection::imuOdomHandler, this, std::placeholders::_1),
            imuOdomOpt);
        subLaserCloud_ = create_subscription<sensor_msgs::msg::PointCloud2>(
            pointCloudTopic, qos_lidar,
            std::bind(&ImageProjection::cloudHandler, this, std::placeholders::_1),
            lidarOpt);

        // 发布去畸变后的点云数据
        pubLaserCloudInfo_ = create_publisher<lio_sam_op::msg::CloudInfo>(
            "lio_sam/deskew/cloud_info", qos);

        // 为动态指针和动态数组分配内存
        // 类似pcl::PointCloud<PointType>::Ptr类型的类成员变量，声明时没有进行初始化，直接调用会报错
        allocateMemory();
        resetParameters();

        pcl::console::setVerbosityLevel(pcl::console::L_ERROR);
    }

    /// @brief 为动态指针和动态数组分配内存
    void allocateMemory()
    {
        laserCloudIn_.reset(new pcl::PointCloud<PointXYZIRT>());
        tmpOusterCloudIn_.reset(new pcl::PointCloud<OusterPointXYZIRT>());
        fullCloud_.reset(new pcl::PointCloud<PointType>());
        extractedCloud_.reset(new pcl::PointCloud<PointType>());

        // 最大的点云数量不会超过N_SACN*Horizon_SCAN
        fullCloud_->points.resize(N_SCAN * Horizon_SCAN);

        /*
        在进行点云去畸变时，把range（2D）数据展开成一维向量
        ring代表第几条激光线数，比如16线的激光雷达有16个ring
        start_ring_index记录第一个ring在一维向量中的起始索引
        end_ring_index记录第一个ring在一维向量中的结束索引
        */
        cloudInfo_.start_ring_index.assign(N_SCAN, 0);
        cloudInfo_.end_ring_index.assign(N_SCAN, 0);

        // 记录一维的Range数据中每一个点在原始range图片中属于那一个列
        cloudInfo_.point_col_ind.assign(N_SCAN * Horizon_SCAN, 0);
        cloudInfo_.point_range.assign(N_SCAN * Horizon_SCAN, 0);

        resetParameters();
    }

    /// @brief 重置各项参数，注意，这部分代码的执行逻辑是接收一帧雷达数据就进行一次完整的处理
    /// 因此，每次处理完都会调用这个函数清空缓存变量
    void resetParameters()
    {
        laserCloudIn_->clear();
        extractedCloud_->clear();

        // 清空点云的range图像
        rangeMat_ = cv::Mat(N_SCAN, Horizon_SCAN, CV_32F, cv::Scalar::all(FLT_MAX));

        imuPointerCur_ = 0;
        firstPointFlag_ = true;
        odomDeskewFlag_ = false;

        // 清空imu数据，这些队列记录当前雷达帧起止时间段内经过计算的IMU数据
        memset(imuTime_, 0, queueLength * sizeof(double));
        memset(imuRotX_, 0, queueLength * sizeof(double));
        memset(imuRotY_, 0, queueLength * sizeof(double));
        memset(imuRotZ_, 0, queueLength * sizeof(double));
    }

    ~ImageProjection() {}

    /// @brief IMU原始数据监听器的回调函数，主要功能是将IMU原始数据对齐到Lidar坐标系，塞入缓存队列
    /// @param imuMsg
    // sensor_msgs::msg::Imu消息包括四元数表示的orientation，还有角速度和线速度
    void imuHandler(const sensor_msgs::msg::Imu::SharedPtr imuMsg)
    {
        // 将IMU坐标系下的IMU数据转换到雷达坐标系（做旋转对齐）
        sensor_msgs::msg::Imu thisImu = imuConverter(*imuMsg);

        std::lock_guard<std::mutex> lock1(imuLock_);
        imuQueue_.push_back(thisImu);

        // 调试IMU坐标系和雷达坐标系对齐时可以打开下面这段代码
        // static int count = 0;
        // if (count%100==0){
        //     // debug IMU data
        //     cout << std::setprecision(6);
        //     cout << "IMU acc: " << endl;
        //     cout << "x: " << thisImu.linear_acceleration.x <<
        //         ", y: " << thisImu.linear_acceleration.y <<
        //         ", z: " << thisImu.linear_acceleration.z << endl;
        //     cout << "IMU gyro: " << endl;
        //     cout << "x: " << thisImu.angular_velocity.x <<
        //         ", y: " << thisImu.angular_velocity.y <<
        //         ", z: " << thisImu.angular_velocity.z << endl;
        //     double imuRoll, imuPitch, imuYaw;
        //     tf2::Quaternion orientation;
        //     tf2::fromMsg(thisImu.orientation, orientation);
        //     tf2::Matrix3x3(orientation).getRPY(imuRoll, imuPitch, imuYaw);
        //     cout << "IMU roll pitch yaw: " << endl;
        //     cout << "roll: " << pcl::rad2deg(imuRoll) << ", pitch: " << pcl::rad2deg(imuPitch) << ", yaw: " << pcl::rad2deg(imuYaw) << endl << endl;
        // }
        // count++;
    }

    /// @brief IMU里程计话题的回调函数，来自imuPreintegration发布的IMU里程计
    // nav_msgs::msg::Odometry包括相对于固定的世界坐标系的位姿消息、相对于child坐标系的线速度和角速度
    void imuOdomHandler(const nav_msgs::msg::Odometry::SharedPtr odometryMsg)
    {
        std::lock_guard<std::mutex> lock2(odoLock_);
        imuOdomQueue_.push_back(*odometryMsg);
    }

    /** 原始雷达点云话题的回调函数，本函数后面的所有函数都是为本函数服务，处理流程如下：
     * 1、转换点云为统一格式，提取点云信息
     *   1）添加一帧激光点云到队列，取出最早一帧作为当前帧
     *   2) 计算起止时间戳，检查数据有效性
     * 2、从IMU数据和IMU里程计数据中提取去畸变信息
     *   1) 遍历当前激光帧起止时刻之间的imu数据，初始时刻对应imu的姿态角RPY设为当前帧的初始姿态角
     *   2) 用角速度、时间积分，计算每一时刻相对于初始时刻的旋转量，初始时刻旋转设为0
     *   imu里程计数据：
     *   1) 遍历当前激光帧起止时刻之间的imu里程计数据，初始时刻对应imu里程计设为当前帧的初始位姿
     *   2) 用起始、终止时刻对应imu里程计，计算相对位姿变换，保存平移增量
     * 3、当前帧激光点云运动畸变校正
     *   1) 检查激光点距离、扫描线是否合规
     *   2) 激光运动畸变校正，保存激光点
     * 4、提取有效激光点，集合信息到准备发布的cloud_info数据包
     * 5、发布当前帧校正后点云，有效点和其他信息
     * 6、重置参数，接收每帧lidar数据都要重置这些参数
     **/
    void cloudHandler(const sensor_msgs::msg::PointCloud2::SharedPtr laserCloudMsg)
    {
        // 1、提取、转换点云为统一格式
        if (!cachePointCloud(laserCloudMsg))
            return;

        // 2、从IMU数据和IMU里程计数据中提取去畸变信息
        if (!deskewInfo())
            return;

        // 3、当前帧激光点云运动畸变校正
        projectPointCloud();

        // 4、提取有效激光点，集合信息到准备发布的cloud_info数据包
        cloudExtraction();

        // 5、发布当前帧校正后点云，有效点和其他信息
        publishClouds();

        // 6、重置参数，接收每帧lidar数据都要重置这些参数
        resetParameters();
    }

    /**
     * @brief 1、转换点云为统一格式，提取点云信息
     *   1）添加一帧激光点云到队列，取出最早一帧作为当前帧
     *   2) 计算起止时间戳，检查数据有效性
     */
    bool cachePointCloud(const sensor_msgs::msg::PointCloud2::SharedPtr &laserCloudMsg)
    {
        // 这里在cloudQueue_.size() <=2 时返回似乎没有太大作用，这个函数是在单线程执行器中被执行
        cloudQueue_.push_back(*laserCloudMsg);
        if (cloudQueue_.size() <= 2)
            return false;

        // 从点云消息队列中取出最早的数据。使用std::move将对象转为右值引用，调用移动赋值运算符，避免拷贝
        currentCloudMsg_ = std::move(cloudQueue_.front());
        cloudQueue_.pop_front();

        // 将点云消息县转换成统一的laserCloudIn格式
        if (sensor == SensorType::VELODYNE)
        {
            pcl::moveFromROSMsg(currentCloudMsg_, *laserCloudIn_);
        }
        else if (sensor == SensorType::OUSTER)
        {
            // 先使用moveFromROSMsg将ouster的PointCloud2信息包转成PCL格式
            pcl::moveFromROSMsg(currentCloudMsg_, *tmpOusterCloudIn_);
            // 将字段名称统一转成Velodyne的格式
            laserCloudIn_->points.resize(tmpOusterCloudIn_->size());
            laserCloudIn_->is_dense = tmpOusterCloudIn_->is_dense;
            for (size_t i = 0; i < tmpOusterCloudIn_->size(); i++)
            {
                auto &src = tmpOusterCloudIn_->points[i];
                auto &dst = laserCloudIn_->points[i];
                dst.x = src.x;
                dst.y = src.y;
                dst.z = src.z;
                dst.intensity = src.intensity;
                dst.ring = src.ring;
                // 每个点的time字段记录的是该点距离该帧点云扫描起始时间的相对时间间隔
                dst.time = src.t * 1e-9f;
            }
        }
        else
        {
            RCLCPP_ERROR_STREAM(get_logger(), "Unknown sensor type: " << int(sensor));
            rclcpp::shutdown();
        }

        // 计算当前帧点云的起始和结束时间
        // 起始时间使用点云message中header时间戳
        // 结束时间通过计算最后一个点相对于起始的时间间隔加上该帧起始时间得到
        cloudHeader_ = currentCloudMsg_.header;
        timeScanCur_ = stamp2Sec(cloudHeader_.stamp);
        timeScanEnd_ = timeScanCur_ + laserCloudIn_->points.back().time;

        // 下面的几段代码都是检查点云的格式是否符合要求，例如ring字段和time字段
        // 但是一些其他激光雷达的ROS节点可能不符合下面的要求，因此没法直接集成到LIOSAM代码中
        // 检查点云是否是密集点云（已经去除无效NaN点）
        if (laserCloudIn_->is_dense == false)
        {
            RCLCPP_ERROR(get_logger(), "点云非稠密类型, 请首先移除NaN点!");
            rclcpp::shutdown();
        }

        // 是否包含ring字段
        // 点云的ring字段标明该点是由第几条激光线束成像的
        // LIOSAM中通过一条ring上的点之间的几何关系计算曲率，提取特征点
        static int ringFlag = 0;
        if (ringFlag == 0)
        {
            ringFlag = -1;
            for (int i = 0; i < (int)currentCloudMsg_.fields.size(); ++i)
            {
                if (currentCloudMsg_.fields[i].name == "ring")
                {
                    ringFlag = 1;
                    break;
                }
            }
            if (ringFlag == -1)
            {
                RCLCPP_ERROR(get_logger(), "点云ring字段不可用, 请合理配置点云数据!");
                rclcpp::shutdown();
            }
        }

        // 是否包含time字段
        // 每个点的time字段用来检索去畸变信息，因此必不可少
        if (deskewFlag_ == 0)
        {
            deskewFlag_ = -1;
            for (auto &field : currentCloudMsg_.fields)
            {
                if (field.name == "time" || field.name == "t")
                {
                    deskewFlag_ = 1;
                    break;
                }
            }
            if (deskewFlag_ == -1)
                RCLCPP_WARN(get_logger(), "点云时间戳不可用，去畸变功能不可用，系统将产生明显的偏移!");
        }

        return true;
    }

    /**
     * 2、从IMU数据和IMU里程计数据中提取去畸变信息
     * imu数据：
     *   1) 遍历当前激光帧起止时刻之间的imu数据，初始时刻对应imu的姿态角RPY设为当前帧的初始姿态角
     *   2) 用角速度、时间积分，计算每一时刻相对于初始时刻的旋转量，初始时刻旋转设为0
     * imu里程计数据：
     *   1) 遍历当前激光帧起止时刻之间的imu里程计数据，初始时刻对应imu里程计设为当前帧的初始位姿
     *   2) 用起始、终止时刻对应imu里程计，计算相对位姿变换，保存平移增量
     **/
    bool deskewInfo()
    {
        // 注意这两把锁，确保了在提取信息的时候IMU队列和里程计队列的固定
        std::lock_guard<std::mutex> lock1(imuLock_);
        std::lock_guard<std::mutex> lock2(odoLock_);

        // 确保目前IMU队列中缓存的IMU数据能够覆盖当前帧的起止时间
        // 由于去畸变部分代码必须依赖高频的IMU，所以如果这里的检查没有通过，当前点云帧会被跳过
        if (imuQueue_.empty() ||
            stamp2Sec(imuQueue_.front().header.stamp) > timeScanCur_ ||
            stamp2Sec(imuQueue_.back().header.stamp) < timeScanEnd_)
        {
            RCLCPP_INFO(get_logger(), "Waiting for IMU data ...");
            return false;
        }

        // 从IMU队列中计算去畸变信息
        imuDeskewInfo();

        // 从IMU里程计中计算去畸变信息
        odomDeskewInfo();

        return true;
    }

    /**
     * 2-1 从IMU数据中提取去畸变信息
     * imu数据：
     *   1) 遍历当前激光帧起止时刻之间的imu数据，初始时刻对应imu的姿态角RPY设为当前帧的初始姿态角
     *   2) 用角速度、时间积分，计算每一时刻相对于初始时刻的旋转量，初始时刻旋转设为0
     * **/
    void imuDeskewInfo()
    {
        // cloudInfo_ 是最终要发布出去的话题数据，具体内容参考格式.msg文件
        // 先把imu标志位设为false，等到imu数据处理完可用才设为true
        cloudInfo_.imu_available = false;

        // 这个while循环可以理解为做IMU数据和点云数据时间戳对齐，不过是一种十分简化的做法。
        // 在各个数据源没有做硬件触发对齐的情况下，这不免是一种很好的做法
        while (!imuQueue_.empty())
        {
            if (stamp2Sec(imuQueue_.front().header.stamp) < timeScanCur_ - 0.01)
                imuQueue_.pop_front();
            else
                break;
        }
        if (imuQueue_.empty())
            return;

        // 下面这个for循环用来做IMU加速度和角速度的积分
        // 假设点云频率为10HZ，则每一帧的扫描时间大概是100ms，时间非常短，因此这里采用的是积分的近似算法
        imuPointerCur_ = 0;
        for (int i = 0; i < (int)imuQueue_.size(); ++i)
        {
            sensor_msgs::msg::Imu thisImuMsg = imuQueue_[i];
            double currentImuTime = stamp2Sec(thisImuMsg.header.stamp);

            // 从9轴IMU数据中提取当前帧起始时刻的roll、pitch、yaw
            if (currentImuTime <= timeScanCur_)
                imuRPY2rosRPY(&thisImuMsg, &cloudInfo_.imu_roll_init, &cloudInfo_.imu_pitch_init, &cloudInfo_.imu_yaw_init);
            // 当前帧对应的IMU数据处理完成
            if (currentImuTime > timeScanEnd_ + 0.01)
                break;

            if (imuPointerCur_ == 0)
            {
                imuRotX_[0] = 0;
                imuRotY_[0] = 0;
                imuRotZ_[0] = 0;
                imuTime_[0] = currentImuTime;
                ++imuPointerCur_;
                continue;
            }

            // 从IMU数据中获取角速度，对角速度的积分变成角度
            static double angular_x, angular_y, angular_z;
            imuAngular2rosAngular(&thisImuMsg, &angular_x, &angular_y, &angular_z);

            // 对角速度做积分，角速度的积分不是简单的角速度乘以间隔时间
            // 关于角速度的积分公式可以查阅：https://zhuanlan.zhihu.com/p/591613108
            static double timeDiff;
            timeDiff = currentImuTime - imuTime_[imuPointerCur_ - 1];
            imuRotX_[imuPointerCur_] = imuRotX_[imuPointerCur_ - 1] + angular_x * timeDiff;
            imuRotY_[imuPointerCur_] = imuRotY_[imuPointerCur_ - 1] + angular_y * timeDiff;
            imuRotZ_[imuPointerCur_] = imuRotZ_[imuPointerCur_ - 1] + angular_z * timeDiff;
            imuTime_[imuPointerCur_] = currentImuTime;
            ++imuPointerCur_;
        }

        --imuPointerCur_;

        if (imuPointerCur_ <= 0)
            return;

        // IMU数据处理成功， 这个标志位标志在图优化中可以使用IMU的角度输出作为该帧点云的初始估计位置
        // 注意，只是标志后续节点可以使用该IMU初始信息，不一定会被使用
        cloudInfo_.imu_available = true;
    }

    /**
     * 2-2 从IMU里程计数据中提取去畸变信息
     * imu里程计数据：
     *   1) 遍历当前激光帧起止时刻之间的imu里程计数据，初始时刻对应imu里程计设为当前帧的初始位姿
     *   2) 用起始、终止时刻对应imu里程计，计算相对位姿变换，保存平移增量
     * **/
    void odomDeskewInfo()
    {
        // 设置IMU里程计可用标志位为false，处理完成才设为true
        cloudInfo_.odom_available = false;

        // 与处理IMU时一样，这个While循环是用来做IMU里程计和雷达点云时间戳对齐的
        // 去除了点云开始时间前面0.01秒之前的IMU里程计数据
        while (!imuOdomQueue_.empty())
        {
            if (stamp2Sec(imuOdomQueue_.front().header.stamp) < timeScanCur_ - 0.01)
                imuOdomQueue_.pop_front();
            else
                break;
        }

        // 如果没有可用的IMU里程计数据，直接返回
        if (imuOdomQueue_.empty())
            return;

        // 再次检查数据时间戳，确保起始的IMU里程计数据在雷达点云开始时间之前
        if (stamp2Sec(imuOdomQueue_.front().header.stamp) > timeScanCur_)
            return;

        // 获取点云起始时刻对应的IMU里程计数据
        nav_msgs::msg::Odometry startOdomMsg;
        for (int i = 0; i < (int)imuOdomQueue_.size(); ++i)
        {
            startOdomMsg = imuOdomQueue_[i];

            if (stamp2Sec(startOdomMsg.header.stamp) < timeScanCur_)
                continue;
            else
                break;
        }

        // 将点云起始时刻对应的IMU里程计数据提取，塞到cloudInfo中
        // initial_guess_等可以被图优化部分当作该帧点云初始位姿
        tf2::Quaternion orientation;
        tf2::fromMsg(startOdomMsg.pose.pose.orientation, orientation);
        double roll, pitch, yaw;
        tf2::Matrix3x3(orientation).getRPY(roll, pitch, yaw);

        cloudInfo_.initial_guess_x = startOdomMsg.pose.pose.position.x;
        cloudInfo_.initial_guess_y = startOdomMsg.pose.pose.position.y;
        cloudInfo_.initial_guess_z = startOdomMsg.pose.pose.position.z;
        cloudInfo_.initial_guess_roll = roll;
        cloudInfo_.initial_guess_pitch = pitch;
        cloudInfo_.initial_guess_yaw = yaw;

        cloudInfo_.odom_available = true;

        // 是否使用IMU里程计数据对点云去畸变的标志位
        // 前面找到了点云起始时刻的IMU里程计，只有在同时找到结束时刻的IMU里程计数据，才可以
        // 利用时间插值算出每一个点对应时刻的位姿做去畸变
        // 注意：！在官方代码中，最后并没有使用IMU里程计数据做去畸变。所以这个标志位世纪没有被使用，
        // 下面的这些代码实际上也没有被使用到
        // 为什么没有使用IMU里程计做去畸变处理？
        // - 原代码中的注释写的是速度较低的情况下不需要做平移去畸变
        odomDeskewFlag_ = false;
        if (stamp2Sec(imuOdomQueue_.back().header.stamp) < timeScanEnd_)
            return;
        nav_msgs::msg::Odometry endOdomMsg;
        for (int i = 0; i < (int)imuOdomQueue_.size(); ++i)
        {
            endOdomMsg = imuOdomQueue_[i];

            if (stamp2Sec(endOdomMsg.header.stamp) < timeScanEnd_)
                continue;
            else
                break;
        }
        if (int(round(startOdomMsg.pose.covariance[0])) != int(round(endOdomMsg.pose.covariance[0])))
            return;

        // 下面这段代码从 startOdomMsg和endOdomMsg计算该帧点云开始和结束之间的位姿变化
        Eigen::Affine3f transBegin = pcl::getTransformation(startOdomMsg.pose.pose.position.x, startOdomMsg.pose.pose.position.y, startOdomMsg.pose.pose.position.z, roll, pitch, yaw);
        tf2::fromMsg(endOdomMsg.pose.pose.orientation, orientation);
        tf2::Matrix3x3(orientation).getRPY(roll, pitch, yaw);
        Eigen::Affine3f transEnd = pcl::getTransformation(endOdomMsg.pose.pose.position.x, endOdomMsg.pose.pose.position.y, endOdomMsg.pose.pose.position.z, roll, pitch, yaw);

        // 求出点云从End坐标系转换到Start坐标系的变换
        // 作者一开始应该是用IMU里程计计算位移变换，所以真正有用的是odomIncreXYZ
        Eigen::Affine3f transBt = transBegin.inverse() * transEnd;
        float rollIncre, pitchIncre, yawIncre;
        pcl::getTranslationAndEulerAngles(transBt, odomIncreX_, odomIncreY_, odomIncreZ_, rollIncre, pitchIncre, yawIncre);
        // 设置里程计去畸变标签为true，标志后面可以用IMU里程计数据做去畸变
        odomDeskewFlag_ = true;
    }

    /**
     * @brief 根据某一个点的时间戳从IMU去畸变信息列表中找到对应的旋转角
     * @param pointTime 点云某一个点的时间戳，秒
     * @param rotXCur 输出的roll角
     * @param rotYCur 输出的pitch角
     * @param rotZCur 输出的yaw角
     */
    void findRotation(double pointTime, float *rotXCur, float *rotYCur, float *rotZCur)
    {
        *rotXCur = 0;
        *rotYCur = 0;
        *rotZCur = 0;

        // imuPointerCur是一个全局变量，表示imuTime_[]、imuRotX_ Y_ Z_[] 这几个队列的实际数据长度
        int imuPointerFront = 0;
        while (imuPointerFront < imuPointerCur_)
        {
            if (pointTime < imuTime_[imuPointerFront])
                break;
            ++imuPointerFront;
        }

        // imuPointerFront为找到的IMU去畸变信息的索引
        // 引起if条件主要是因为可能最后一个IMU的时间戳依然是小于雷达点的时间戳，
        // 这个时候就直接使用最后一个IMU对应的角度
        if (pointTime > imuTime_[imuPointerFront] || imuPointerFront == 0)
        {
            *rotXCur = imuRotX_[imuPointerFront];
            *rotYCur = imuRotY_[imuPointerFront];
            *rotZCur = imuRotZ_[imuPointerFront];
        }
        // else 部分根据时间戳比例进行角度插值，计算出更准确的旋转
        else
        {
            int imuPointerBack = imuPointerFront - 1;
            double ratioFront = (pointTime - imuTime_[imuPointerBack]) / (imuTime_[imuPointerFront] - imuTime_[imuPointerBack]);
            double ratioBack = (imuTime_[imuPointerFront] - pointTime) / (imuTime_[imuPointerFront] - imuTime_[imuPointerBack]);
            *rotXCur = imuRotX_[imuPointerFront] * ratioFront + imuRotX_[imuPointerBack] * ratioBack;
            *rotYCur = imuRotY_[imuPointerFront] * ratioFront + imuRotY_[imuPointerBack] * ratioBack;
            *rotZCur = imuRotZ_[imuPointerFront] * ratioFront + imuRotZ_[imuPointerBack] * ratioBack;
        }
    }

    /**
     * @brief 根据某一个点的时间戳从IMU里程计去畸变信息列表中找到对应的平移。
     * 这个函数根据时间间隔对平移量进行插值得到起止时间段之间任意时刻的位移
     *
     * @param relTime 点云中某一个点的time字段（相对于起始点的时间）
     * @param poseXCur 输出的X轴方向平移量
     * @param poseYCur 输出的Y轴方向平移量
     * @param poseZCur 输出的Z轴方向平移量
     */
    void findPosition(double relTime, float *posXCur, float *posYCur, float *posZCur)
    {
        *posXCur = 0;
        *posYCur = 0;
        *posZCur = 0;

        // If the sensor moves relatively slow, like walking speed, positional deskew seems to have little benefits. Thus code below is commented.

        // if (cloudInfo_.odomAvailable == false || odomDeskewFlag_ == false)
        //     return;

        // float ratio = relTime / (timeScanEnd_ - timeScanCur_);

        // *posXCur = ratio * odomIncreX_;
        // *posYCur = ratio * odomIncreY_;
        // *posZCur = ratio * odomIncreZ_;
    }

    /**
     * @brief
     * 3-2 雷达单点运动畸变矫正
     *    1. 找到该点对应的旋转
     *    2. 找到该点对应的平移
     *    3. 坐标变换
     *
     * @param point 点云中某个点位置
     * @param relTime 该点相对于该帧点云起始时刻的相对时间
     *
     * @return 去完畸变后的点
     *
     */
    PointType deskewPoint(PointType *point, double relTime)
    {
        // 如果IMU去畸变信息或者IMU里程计去畸变信息不可用，返回原始点
        if (deskewFlag_ == -1 || cloudInfo_.imu_available == false)
            return *point;

        // 计算该点的绝对时间
        double pointTime = timeScanCur_ + relTime;

        // 找到旋转量
        float rotXCur, rotYCur, rotZCur;
        findRotation(pointTime, &rotXCur, &rotYCur, &rotZCur);

        // 找到平移量， 这里始终是0，0,0
        float posXCur, posYCur, posZCur;
        findPosition(relTime, &posXCur, &posYCur, &posZCur);

        // 缓存第一个点对应的位姿
        if (firstPointFlag_ == true)
        {
            transStartInverse_ = (pcl::getTransformation(posXCur, posYCur, posZCur, rotXCur, rotYCur, rotZCur)).inverse();
            firstPointFlag_ = false;
        }

        // 计算当前点到起始点的变换矩阵
        Eigen::Affine3f transFinal = pcl::getTransformation(posXCur, posYCur, posZCur, rotXCur, rotYCur, rotZCur);
        Eigen::Affine3f transBt = transStartInverse_ * transFinal;
        // 三维点坐标变换，这里可以直接调用PCL的接口
        PointType newPoint;
        newPoint.x = transBt(0, 0) * point->x + transBt(0, 1) * point->y + transBt(0, 2) * point->z + transBt(0, 3);
        newPoint.y = transBt(1, 0) * point->x + transBt(1, 1) * point->y + transBt(1, 2) * point->z + transBt(1, 3);
        newPoint.z = transBt(2, 0) * point->x + transBt(2, 1) * point->y + transBt(2, 2) * point->z + transBt(2, 3);
        newPoint.intensity = point->intensity;

        return newPoint;
    }

    /**
     * @brief
     * 3、执行点云去畸变
     *  1. 检查激光点距离、扫描线是否合规
     *  2. 单点去畸变
     *  3. 保存激光点
     */
    void projectPointCloud()
    {
        int cloudSize = laserCloudIn_->points.size();
        // range image projection
        for (int i = 0; i < cloudSize; ++i)
        {
            PointType thisPoint;
            thisPoint.x = laserCloudIn_->points[i].x;
            thisPoint.y = laserCloudIn_->points[i].y;
            thisPoint.z = laserCloudIn_->points[i].z;
            thisPoint.intensity = laserCloudIn_->points[i].intensity;

            // range从点云的x、y、z算出来，而没有使用topic信息中的range
            // range映射成2D矩阵时
            // 行索引rowInd等于该点的ring
            // 列索引根据角度和分辨率计算得到
            float range = pointDistance(thisPoint);
            if (range < lidarMinRange || range > lidarMaxRange)
                continue;

            int rowIdn = laserCloudIn_->points[i].ring;
            if (rowIdn < 0 || rowIdn >= N_SCAN)
                continue;
            // 可以执行线束级别降采样
            if (rowIdn % downsampleRate != 0)
                continue;

            // 计算该点对应的列索引
            float horizonAngle = atan2(thisPoint.x, thisPoint.y) * 180 / M_PI;
            static float ang_res_x = 360.0 / float(Horizon_SCAN);
            int columnIdn = -round((horizonAngle - 90.0) / ang_res_x) + Horizon_SCAN / 2;
            if (columnIdn >= Horizon_SCAN)
                columnIdn -= Horizon_SCAN;
            if (columnIdn < 0 || columnIdn >= Horizon_SCAN)
                continue;

            // 将该点的range记录到rangeMat
            if (rangeMat_.at<float>(rowIdn, columnIdn) != FLT_MAX)
                continue;
            rangeMat_.at<float>(rowIdn, columnIdn) = range;

            // 单点进行去畸变
            thisPoint = deskewPoint(&thisPoint, laserCloudIn_->points[i].time);

            // 将去完畸变的点云存储在中间变量
            int index = columnIdn + rowIdn * Horizon_SCAN;
            fullCloud_->points[index] = thisPoint;
        }
    }

    /**
     * @brief 4、提取有效激光点，整理信息到准备发布的cloudInfo数据包
     */
    void cloudExtraction()
    {
        // 有效激光点数量
        int count = 0;
        // extract segmented cloud for lidar odometry
        // 提取特征的时候，每一行（每一条线束）的前5个点和最后5个点不考虑
        // 因为在后面提取特征的时候需要对每个点计算曲率，算法中用该点前后10个点计算，
        // 因此，每一行的前5个点和最后5个点没法计算在提取有效点的时候，把range拉成一维向量，
        // start_ring_index和end_ring_index中存储的是在这个一维向量中的索引。
        for (int i = 0; i < N_SCAN; ++i)
        {
            // 第i行点云在一维向量中的起始索引值
            cloudInfo_.start_ring_index[i] = count - 1 + 5;
            for (int j = 0; j < Horizon_SCAN; ++j)
            {
                if (rangeMat_.at<float>(i, j) != FLT_MAX)
                {
                    // 第count个点列索引
                    // 后面算法用来计算遮挡点用到
                    cloudInfo_.point_col_ind[count] = j;
                    // 记录第count有效点的range
                    cloudInfo_.point_range[count] = rangeMat_.at<float>(i, j);
                    // 记录第count个有效点的点坐标
                    extractedCloud_->push_back(fullCloud_->points[j + i * Horizon_SCAN]);
                    // 有效点数量加1
                    ++count;
                }
            }
            // 第i行点云在一维向量中的结束索引值
            cloudInfo_.end_ring_index[i] = count - 1 - 5;
        }
    }

    /**
     * @brief 发布当前帧校正后的点云以及其他有效信息
     */
    void publishClouds()
    {
        cloudInfo_.header = cloudHeader_;
        // 将前文提取的去畸变后的有效点云extractedCloud, 从pcl格式转成ros message格式
        pclPointcloud2Ros(extractedCloud_, cloudInfo_.cloud_deskewed, cloudHeader_.stamp, cloudHeader_.frame_id);
        pubLaserCloudInfo_->publish(cloudInfo_);
    }
};

int main(int argc, char **argv)
{
    rclcpp::init(argc, argv);

    rclcpp::NodeOptions options;
    options.use_intra_process_comms(true);
    rclcpp::executors::MultiThreadedExecutor exec;

    auto IP = std::make_shared<ImageProjection>(options);
    exec.add_node(IP);

    RCLCPP_INFO(rclcpp::get_logger("rclcpp"), "\033[1;32m----> Image Projection Started.\033[0m");

    exec.spin();

    rclcpp::shutdown();
    return 0;
}
