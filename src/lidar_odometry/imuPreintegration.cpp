#include "utility.h"

#include <gtsam/geometry/Rot3.h>
#include <gtsam/geometry/Pose3.h>
#include <gtsam/slam/PriorFactor.h>
#include <gtsam/slam/BetweenFactor.h>
#include <gtsam/navigation/GPSFactor.h>
#include <gtsam/navigation/ImuFactor.h>
#include <gtsam/navigation/CombinedImuFactor.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/nonlinear/LevenbergMarquardtOptimizer.h>
#include <gtsam/nonlinear/Marginals.h>
#include <gtsam/nonlinear/Values.h>
#include <gtsam/inference/Symbol.h>

#include <gtsam/nonlinear/ISAM2.h>
#include <gtsam_unstable/nonlinear/IncrementalFixedLagSmoother.h>

using gtsam::symbol_shorthand::X; // Pose3 (x,y,z,r,p,y)
using gtsam::symbol_shorthand::V; // Vel   (xdot,ydot,zdot)
using gtsam::symbol_shorthand::B; // Bias  (ax,ay,az,gx,gy,gz)

// IMU预积分的构造函数，继承自ParamServer类
class IMUPreintegration : public ParamServer
{
public:
    //订阅和发布
    ros::Subscriber subImu;
    ros::Subscriber subOdometry;
    ros::Publisher pubImuOdometry;
    ros::Publisher pubImuPath;

    // map -> odom
    tf::Transform map_to_odom;
    tf::TransformBroadcaster tfMap2Odom;
    // odom -> base_link
    tf::TransformBroadcaster tfOdom2BaseLink;

    bool systemInitialized = false;

    //噪声协方差
    gtsam::noiseModel::Diagonal::shared_ptr priorPoseNoise;
    gtsam::noiseModel::Diagonal::shared_ptr priorVelNoise;
    gtsam::noiseModel::Diagonal::shared_ptr priorBiasNoise;
    gtsam::noiseModel::Diagonal::shared_ptr correctionNoise;
    gtsam::Vector noiseModelBetweenBias;

    //imu 预积分器
    gtsam::PreintegratedImuMeasurements *imuIntegratorOpt_;
    gtsam::PreintegratedImuMeasurements *imuIntegratorImu_;

    // imu 数据队列
    std::deque<sensor_msgs::Imu> imuQueOpt;
    std::deque<sensor_msgs::Imu> imuQueImu;
    
    // imu因子图优化过程中的状态变量
    gtsam::Pose3 prevPose_;
    gtsam::Vector3 prevVel_;
    //Navigation state: Pose (rotation, translation) + velocity
    gtsam::NavState prevState_;
    gtsam::imuBias::ConstantBias prevBias_;
    
    // imu状态
    gtsam::NavState prevStateOdom;
    gtsam::imuBias::ConstantBias prevBiasOdom;

    bool doneFirstOpt = false;
    double lastImuT_imu = -1;
    double lastImuT_opt = -1;

    // ISAM2 优化器
    gtsam::ISAM2 optimizer;
    gtsam::NonlinearFactorGraph graphFactors;
    gtsam::Values graphValues;

    const double delta_t = 0;

    int key = 1;
    int imuPreintegrationResetId = 0;

    // imu-lidar位姿变换
    gtsam::Pose3 imu2Lidar = gtsam::Pose3(gtsam::Rot3(1, 0, 0, 0), gtsam::Point3(-extTrans.x(), -extTrans.y(), -extTrans.z()));
    gtsam::Pose3 lidar2Imu = gtsam::Pose3(gtsam::Rot3(1, 0, 0, 0), gtsam::Point3(extTrans.x(), extTrans.y(), extTrans.z()));;

    // 构造函数
    IMUPreintegration()
    {
        // 订阅imu原始数据，用下面因子图优化的结果，施加两帧之间的imu预积分量，预测每一个时刻（imu频率）的imu里程计，五个参数imuTopic-话题名称（通常为 "/imu/data" 或类似名称）、2000-队列长度：最多缓存 2000 条 IMU 消息、&IMUPreintegration::imuHandler-回调函数：接收到消息后调用 imuHandler、this-回调所属的对象指针、ros::TransportHints().tcpNoDelay()-传输设置：禁用 Nagle 算法，降低延迟
        subImu      = nh.subscribe<sensor_msgs::Imu>  (imuTopic, 2000, &IMUPreintegration::imuHandler, this, ros::TransportHints().tcpNoDelay());
        // 订阅激光里程计，来自mapOptimization，用两帧之间的imu预计分量构建因子图，优化当前帧位姿（这个位姿仅用于更新每时刻的imu里程计，以及下一次因子图优化）
        subOdometry = nh.subscribe<nav_msgs::Odometry>(PROJECT_NAME + "/lidar/mapping/odometry", 5, &IMUPreintegration::odometryHandler, this, ros::TransportHints().tcpNoDelay());
        // 发布imu 里程计
        pubImuOdometry = nh.advertise<nav_msgs::Odometry> ("odometry/imu", 2000);
        // 发布imu路径
        pubImuPath     = nh.advertise<nav_msgs::Path>(
            PROJECT_NAME + "/lidar/imu/path", // 话题名称
            1                                 // 发布队列大小
            );

        map_to_odom = tf::Transform(tf::createQuaternionFromRPY(0, 0, 0), tf::Vector3(0, 0, 0));

        // imu 预积分的噪声协方差
        boost::shared_ptr<gtsam::PreintegrationParams> p = gtsam::PreintegrationParams::MakeSharedU(imuGravity);
        // 加速度计白噪声（连续模型）
        p->accelerometerCovariance  = gtsam::Matrix33::Identity(3,3) * pow(imuAccNoise, 2); // acc white noise in continuous
        // 陀螺仪白噪声（连续模型）
        p->gyroscopeCovariance      = gtsam::Matrix33::Identity(3,3) * pow(imuGyrNoise, 2); // gyro white noise in continuous
        // 积分过程的额外噪声
        p->integrationCovariance    = gtsam::Matrix33::Identity(3,3) * pow(1e-4, 2); // error committed in integrating position from velocities
        // 初始 IMU 偏置：假设为零
        gtsam::imuBias::ConstantBias prior_imu_bias((gtsam::Vector(6) << 0, 0, 0, 0, 0, 0).finished());; // assume zero initial bias

        // 噪声先验，定义因子图中各类先验噪声
        // 位姿先验噪声（角度：rad，位置：m）
        priorPoseNoise  = gtsam::noiseModel::Diagonal::Sigmas((gtsam::Vector(6) << 1e-2, 1e-2, 1e-2, 1e-2, 1e-2, 1e-2).finished()); // rad,rad,rad,m, m, m
        // 速度先验噪声（m/s）
        priorVelNoise   = gtsam::noiseModel::Isotropic::Sigma(3, 1e2); // m/s
        // 偏置先验噪声
        priorBiasNoise  = gtsam::noiseModel::Isotropic::Sigma(6, 1e-3); // 1e-2 ~ 1e-3 seems to be good
        // 激光里程计scan-to-map优化过程中发生退化，则选择一个较大的协方差
        correctionNoise = gtsam::noiseModel::Isotropic::Sigma(6, 1e-2); // meter
        noiseModelBetweenBias = (gtsam::Vector(6) << imuAccBiasN, imuAccBiasN, imuAccBiasN, imuGyrBiasN, imuGyrBiasN, imuGyrBiasN).finished();
        // imu 预积分器，用于预测每一时刻（imu频率）的imu里程计（转到lidar系了，与激光里程计同一个坐标系）
        imuIntegratorImu_ = new gtsam::PreintegratedImuMeasurements(p, prior_imu_bias); // setting up the IMU integration for IMU message thread
        // imu 预积分器，用于因子图优化
        imuIntegratorOpt_ = new gtsam::PreintegratedImuMeasurements(p, prior_imu_bias); // setting up the IMU integration for optimization        
    }

    // 重置ISAM2 优化器，当因子图累积到一定规模（代码中是每 100 帧重置一次）或者系统第一次初始化时，需要重置优化器，以控制计算量和数值稳定性
    void resetOptimization()
    {
        // 1. 配置 ISAM2 参数
        gtsam::ISAM2Params optParameters; // 创建一个 ISAM2 参数对象，用来配置增量优化器的行为
        optParameters.relinearizeThreshold = 0.1; // 当估计值与线性化点的偏差超过 0.1（单位为变量的标度），触发对该变量的重线性化
        optParameters.relinearizeSkip = 1;  // 每处理 1 个增量步骤，就检查是否需要重线性化
        
        // 2. 使用新参数初始化 ISAM2 优化器
        optimizer = gtsam::ISAM2(optParameters);  // 根据新参数重新构造 ISAM2 优化器对象，旧的状态和因子都会被丢弃

        // 3. 清空因子图和变量值
        gtsam::NonlinearFactorGraph newGraphFactors;
        graphFactors = newGraphFactors;  // 创建并替换一个空的因子图，清除之前所有添加的因子

        gtsam::Values NewGraphValues;
        graphValues = NewGraphValues;  // 创建并替换一个空的变量值容器，清除之前所有的初始估计值
    }

    // 重置参数
    void resetParams()
    {
        lastImuT_imu = -1;
        doneFirstOpt = false;
        systemInitialized = false;
    }

    /**
     * 订阅激光里程计，来自mapOptimization
     * 1、每隔100帧激光里程计，重置ISAM2优化器，添加里程计、速度、偏置先验因子，执行优化
     * 2、计算前一帧激光里程计与当前帧激光里程计之间的imu预积分量，用前一帧状态施加预积分量得到当前帧初始状态估计，添加来自mapOptimization的当前帧位姿，进行因子图优化，更新当前帧状态
     * 3、优化之后，执行重传播；优化更新了imu的偏置，用最新的偏置重新计算当前激光里程计时刻之后的imu预积分，这个预积分用于计算每时刻位姿
    */
    void odometryHandler(const nav_msgs::Odometry::ConstPtr& odomMsg)
    {
        // 当前激光里程计时间戳
        double currentCorrectionTime = ROS_TIME(odomMsg);

        // make sure we have imu data to integrate
        // 确保imu优化队列中有imu数据进行预积分
        if (imuQueOpt.empty())
            return;

        // 当前帧激光位姿，来自scan-to-map匹配，因子图优化后的位姿
        float p_x = odomMsg->pose.pose.position.x;
        float p_y = odomMsg->pose.pose.position.y;
        float p_z = odomMsg->pose.pose.position.z;
        float r_x = odomMsg->pose.pose.orientation.x;
        float r_y = odomMsg->pose.pose.orientation.y;
        float r_z = odomMsg->pose.pose.orientation.z;
        float r_w = odomMsg->pose.pose.orientation.w;

        // 检查是否需要重置预积分器（里程计跳变）
        int currentResetId = round(odomMsg->pose.covariance[0]);
        gtsam::Pose3 lidarPose = gtsam::Pose3(gtsam::Rot3::Quaternion(r_w, r_x, r_y, r_z), gtsam::Point3(p_x, p_y, p_z)); //构建 LiDAR→IMU 坐标系下的 Pose3

        // correction pose jumped, reset imu pre-integration跳变检测
        if (currentResetId != imuPreintegrationResetId)
        {   
            // 里程计跳变：重置所有状态
            resetParams();
            imuPreintegrationResetId = currentResetId;
            return;
        }


        // 0. initialize system
        // 0. 系统初始化，第一帧
        if (systemInitialized == false)
        {
            // 重置ISAM2优化器和因子图
            resetOptimization();

            // pop old IMU message
            // 从imu优化队列中删除当前激光里程计时刻之前的imu数据
            while (!imuQueOpt.empty())
            {
                if (ROS_TIME(&imuQueOpt.front()) < currentCorrectionTime - delta_t)
                {
                    lastImuT_opt = ROS_TIME(&imuQueOpt.front());
                    imuQueOpt.pop_front();
                }
                else
                    break;
            }


            /*
            消除系统的整体漂移（Gauge Freedom）
            因子图优化本质上是在一个由变量（节点）和约束（因子）构成的图上求最优解。
            如果图中只有“相对”约束（比如 IMU 因子、里程计因子等），那么整体的平移、旋转、速度甚至偏置就没有绝对参考，
            优化结果会出现任意的全局平移或旋转（漂移现象）。
            设定初始条件
            在系统刚启动、还没有任何里程计或 IMU 因子加入的时候，需要给优化器一个“猜测的起点”，让它有东西可以优化。
            数值稳定性与收敛性
            合理设置先验的协方差（噪声水平）能够平衡优化的“信心”——
            协方差设得太大，相当于告诉优化器“对这个先验不太在意”，效果就近似于没有先验，仍然可能导致漂移。
            协方差设得太小，又会让优化器过度“死守”这个先验，难以校正真实的系统偏差。
            因此，通过对先验噪声模型的精细调节，可以兼顾对初始状态的约束和系统后续对新观测数据的灵活响应。
            可扩展性与模块化
            将先验因子作为独立的节点插入，也方便后续做重新初始化或“重锚”——
            当检测到里程计跳变或需要重置时，只要丢弃旧图、重新添加新的先验，就能让整个优化过程快速回到稳健状态，不必重新从头积累几十帧的观测。
            
            位姿先验（PriorFactor<Pose3>(X(0), prevPose_, priorPoseNoise)）就像给第一个位姿打了一根“锚杆”，
            告诉优化器“从这里开始，这个节点的绝对位置大致在这里”，从而锁定全局坐标系。
            速度先验（PriorFactor<Vector3>(V(0), prevVel_, priorVelNoise)）和偏置先验（PriorFactor<imuBias::ConstantBias>(B(0), prevBias_, priorBiasNoise)）
            分别告诉优化器“初速度大约是零”、“初始 IMU 偏置大约是零”，从而确保优化从一个合理的状态开始。
            */

            
            // initial pose
            // 添加里程计位姿先验因子
            prevPose_ = lidarPose.compose(lidar2Imu); // 将当前里程计位姿转换到imu坐标系下，将 lidarPose 转到 IMU 系下，得到 prevPose_，作为变量 X(0) 的先验
            // X(0)表示第一个位姿，有一个先验的约束。约束内容为，lidar到imu下的prevPose_这么一个位姿
            // 该约束的权重，置信度为priorPoseNoise，越小代表置信度越高
            gtsam::PriorFactor<gtsam::Pose3> priorPose(X(0), prevPose_, priorPoseNoise);
            // 约束添加到因子图当中
            graphFactors.add(priorPose);
            // initial velocity
            // 添加速度先验因子，初始速度设为0，注意priorVelNoise非常大
            prevVel_ = gtsam::Vector3(0, 0, 0);
            gtsam::PriorFactor<gtsam::Vector3> priorVel(V(0), prevVel_, priorVelNoise);
            graphFactors.add(priorVel);
            // initial bias
            // 添加imu bias 先验因子，初值为零
            prevBias_ = gtsam::imuBias::ConstantBias();
            gtsam::PriorFactor<gtsam::imuBias::ConstantBias> priorBias(B(0), prevBias_, priorBiasNoise);
            graphFactors.add(priorBias);
            // add values
            // 变量节点赋初值
            graphValues.insert(X(0), prevPose_);
            graphValues.insert(V(0), prevVel_);
            graphValues.insert(B(0), prevBias_);
            // optimize once
            // 进行优化一次
            optimizer.update(graphFactors, graphValues);
            // 送入优化器后保存约束和状态的变量就清零
            graphFactors.resize(0);
            graphValues.clear();

            // 重置优化之后的bias，同步预积分器：保证预测和优化两路预积分都以相同的初始偏置开始
            imuIntegratorImu_->resetIntegrationAndSetBias(prevBias_);
            imuIntegratorOpt_->resetIntegrationAndSetBias(prevBias_);
            
            key = 1;
            // 系统已经进行了初始化的标志
            systemInitialized = true;
            return;
        }


        // reset graph for speed
        // 每隔100帧激光里程计，重置ISAM2优化器，保证优化效率
        if (key == 100)
        {
            // get updated noise before reset
            gtsam::noiseModel::Gaussian::shared_ptr updatedPoseNoise = gtsam::noiseModel::Gaussian::Covariance(optimizer.marginalCovariance(X(key-1)));
            gtsam::noiseModel::Gaussian::shared_ptr updatedVelNoise  = gtsam::noiseModel::Gaussian::Covariance(optimizer.marginalCovariance(V(key-1)));
            gtsam::noiseModel::Gaussian::shared_ptr updatedBiasNoise = gtsam::noiseModel::Gaussian::Covariance(optimizer.marginalCovariance(B(key-1)));
            // reset graph
            resetOptimization();
            // add pose
            gtsam::PriorFactor<gtsam::Pose3> priorPose(X(0), prevPose_, updatedPoseNoise);
            graphFactors.add(priorPose);
            // add velocity
            gtsam::PriorFactor<gtsam::Vector3> priorVel(V(0), prevVel_, updatedVelNoise);
            graphFactors.add(priorVel);
            // add bias
            gtsam::PriorFactor<gtsam::imuBias::ConstantBias> priorBias(B(0), prevBias_, updatedBiasNoise);
            graphFactors.add(priorBias);
            // add values
            graphValues.insert(X(0), prevPose_);
            graphValues.insert(V(0), prevVel_);
            graphValues.insert(B(0), prevBias_);
            // optimize once
            optimizer.update(graphFactors, graphValues);
            graphFactors.resize(0);
            graphValues.clear();

            key = 1;
        }


        // 1. integrate imu data and optimize
        // 1. 计算前一帧与当前帧之间的imu预积分量，用前一帧状态施加预积分量得到当前帧初始状态估计，添加来自mapOptimization的当前帧位姿，进行因子图优化，更新当前帧状态
        while (!imuQueOpt.empty())
        {
            // pop and integrate imu data that is between two optimizations
            // 提取前一帧和当前帧之间的imu数据，计算预积分
            sensor_msgs::Imu *thisImu = &imuQueOpt.front();
            double imuTime = ROS_TIME(thisImu);
            if (imuTime < currentCorrectionTime - delta_t)
            {
                // 两帧imu数据时间间隔
                double dt = (lastImuT_opt < 0) ? (1.0 / 500.0) : (imuTime - lastImuT_opt);
                // imu预积分输入：加速度，角速度，dt
                // gtsam会自动完成预积分量的更新及协方差的更新等操作
                imuIntegratorOpt_->integrateMeasurement(
                        gtsam::Vector3(thisImu->linear_acceleration.x, thisImu->linear_acceleration.y, thisImu->linear_acceleration.z),
                        gtsam::Vector3(thisImu->angular_velocity.x,    thisImu->angular_velocity.y,    thisImu->angular_velocity.z), dt);
                
                lastImuT_opt = imuTime;
                // 队列中删除已经处理过的imu数据
                imuQueOpt.pop_front();
            }
            else
                break;
        }
        // add imu factor to graph
        // 添加imu预积分因子
        const gtsam::PreintegratedImuMeasurements& preint_imu = dynamic_cast<const gtsam::PreintegratedImuMeasurements&>(*imuIntegratorOpt_);
        // 参数：前一帧位姿，前一帧速度，当前帧位姿，当前帧速度，前一阵bias，预积分量
        gtsam::ImuFactor imu_factor(X(key - 1), V(key - 1), X(key), V(key), B(key - 1), preint_imu);
        graphFactors.add(imu_factor);
        // add imu bias between factor
        // 添加imu偏置因子，前一帧偏置，当前帧偏置，观测值，噪声协方差；deltaTij()是积分段的时间
        graphFactors.add(gtsam::BetweenFactor<gtsam::imuBias::ConstantBias>(B(key - 1), B(key), gtsam::imuBias::ConstantBias(),
                         gtsam::noiseModel::Diagonal::Sigmas(sqrt(imuIntegratorOpt_->deltaTij()) * noiseModelBetweenBias)));
        // add pose factor
        // 添加位姿因子
        gtsam::Pose3 curPose = lidarPose.compose(lidar2Imu);
        // 对于 LIO-SAM ，如果退化就让置信度小一些
        // LIO-SAM 此处代码为：
        // gtsam::PriorFactor<gtsam::Pose3> pose_factor(X(key), curPose, degenerate ? correctionNoise2 : correctionNoise);
        gtsam::PriorFactor<gtsam::Pose3> pose_factor(X(key), curPose, correctionNoise);
        graphFactors.add(pose_factor);
        // insert predicted values
        // 用前一帧的状态和bias，施加imu预积分量，得到当前帧的状态
        gtsam::NavState propState_ = imuIntegratorOpt_->predict(prevState_, prevBias_);
        // 预测量作为初始值插入到因子图当中
        graphValues.insert(X(key), propState_.pose());
        graphValues.insert(V(key), propState_.v());
        graphValues.insert(B(key), prevBias_);
        // optimize
        // 优化两次
        optimizer.update(graphFactors, graphValues);
        optimizer.update();
        graphFactors.resize(0);
        graphValues.clear();
        // Overwrite the beginning of the preintegration for the next step.
        // 优化结果，获取优化后的当前状态作为当前帧的最佳估计
        gtsam::Values result = optimizer.calculateEstimate();
        // 更新当前帧位姿，速度
        prevPose_  = result.at<gtsam::Pose3>(X(key));
        prevVel_   = result.at<gtsam::Vector3>(V(key));
        // 更新当前帧状态
        prevState_ = gtsam::NavState(prevPose_, prevVel_);
        // 更新当前帧imu bias
        prevBias_  = result.at<gtsam::imuBias::ConstantBias>(B(key));
        // Reset the optimization preintegration object.
        // 预积分器复位，设置新的偏置
        imuIntegratorOpt_->resetIntegrationAndSetBias(prevBias_);
        // check optimization
        // 检查imu因子图优化结果，如果速度或者bias过大，认为其失败
        if (failureDetection(prevVel_, prevBias_))
        {
            // 失败后重置参数
            resetParams();
            return;
        }


        // 2. after optiization, re-propagate imu odometry preintegration
        // 2. 优化之后，执行重传播；优化更新了imu的偏置，用最新的偏置重新计算当前激光里程计时刻之后的imu预积分，这个预积分用于计算每时刻位姿
        prevStateOdom = prevState_;
        prevBiasOdom  = prevBias_;
        // first pop imu message older than current correction data
        // 从 imu 队列中删除当前激光里程计时刻之前的imu数据
        double lastImuQT = -1;
        while (!imuQueImu.empty() && ROS_TIME(&imuQueImu.front()) < currentCorrectionTime - delta_t)
        {
            lastImuQT = ROS_TIME(&imuQueImu.front());
            imuQueImu.pop_front();
        }
        // repropogate
        // 对剩余的imu数据计算预积分
        if (!imuQueImu.empty())
        {
            // reset bias use the newly optimized bias
            // 用最近的bias来重置预积分器
            imuIntegratorImu_->resetIntegrationAndSetBias(prevBiasOdom);
            // integrate imu message from the beginning of this optimization
            // 计算预积分
            for (int i = 0; i < (int)imuQueImu.size(); ++i)
            {
                sensor_msgs::Imu *thisImu = &imuQueImu[i];
                double imuTime = ROS_TIME(thisImu);
                double dt = (lastImuQT < 0) ? (1.0 / 500.0) :(imuTime - lastImuQT);

                imuIntegratorImu_->integrateMeasurement(gtsam::Vector3(thisImu->linear_acceleration.x, thisImu->linear_acceleration.y, thisImu->linear_acceleration.z),
                                                        gtsam::Vector3(thisImu->angular_velocity.x,    thisImu->angular_velocity.y,    thisImu->angular_velocity.z), dt);
                lastImuQT = imuTime;
            }
        }

        ++key;
        doneFirstOpt = true;
    }

    bool failureDetection(const gtsam::Vector3& velCur, const gtsam::imuBias::ConstantBias& biasCur)
    {
        Eigen::Vector3f vel(velCur.x(), velCur.y(), velCur.z());
        if (vel.norm() > 30)
        {
            ROS_WARN("Large velocity, reset IMU-preintegration!");
            return true;
        }

        Eigen::Vector3f ba(biasCur.accelerometer().x(), biasCur.accelerometer().y(), biasCur.accelerometer().z());
        Eigen::Vector3f bg(biasCur.gyroscope().x(), biasCur.gyroscope().y(), biasCur.gyroscope().z());
        if (ba.norm() > 0.1 || bg.norm() > 0.1)
        {
            ROS_WARN("Large bias, reset IMU-preintegration!");
            return true;
        }

        return false;
    }

    /**
     * 订阅imu原始数据
     * 1、用上一帧激光里程计时刻对应的状态、偏置，施加从该时刻开始到当前时刻的imu预计分量，得到当前时刻的状态，也就是imu里程计
     * 2、imu里程计位姿转到lidar系，发布里程计
    */
    void imuHandler(const sensor_msgs::Imu::ConstPtr& imuMsg)
    {
        
        // imu原始测量数据转换到lidar系（加速度，角速度，RPY）
        sensor_msgs::Imu thisImu = imuConverter(*imuMsg);
        // publish static tf
        tfMap2Odom.sendTransform(tf::StampedTransform(map_to_odom, thisImu.header.stamp, "map", "odom"));
        // 添加当前帧imu数据到队列
        imuQueOpt.push_back(thisImu);
        imuQueImu.push_back(thisImu);

        // 要求上一次imu因子图优化执行成功，确保更新了上一帧（激光里程计帧）的状态和bias
        if (doneFirstOpt == false)
            return;

        double imuTime = ROS_TIME(&thisImu);
        // 更新当前imu数据和上次imu数据的时间间隔dt，如果是第一次就认为 dt = 1 / 500.0 
        
        double dt = (lastImuT_imu < 0) ? (1.0 / 500.0) : (imuTime - lastImuT_imu);
        lastImuT_imu = imuTime;

        // integrate this single imu message
        // imu预积分器添加一帧imu数据，这个预积分的起始时刻是上一帧激光里程计时刻
        imuIntegratorImu_->integrateMeasurement(gtsam::Vector3(thisImu.linear_acceleration.x, thisImu.linear_acceleration.y, thisImu.linear_acceleration.z),
                                                gtsam::Vector3(thisImu.angular_velocity.x,    thisImu.angular_velocity.y,    thisImu.angular_velocity.z), dt);

        // predict odometry
        // 用上一帧激光里程计时刻对应的状态和bias，施加从该时刻开始到当前时刻的imu预积分量，得到当前时刻的状态
        // 这里的值是优化过后的prevState
        gtsam::NavState currentState = imuIntegratorImu_->predict(prevStateOdom, prevBiasOdom);

        // publish odometry
        // 发布imu里程计（注意与激光里程计同一个系）
        nav_msgs::Odometry odometry;
        odometry.header.stamp = thisImu.header.stamp;
        odometry.header.frame_id = "odom";
        odometry.child_frame_id = "odom_imu";

        // transform imu pose to ldiar
        // 变换到lidar坐标系
        gtsam::Pose3 imuPose = gtsam::Pose3(currentState.quaternion(), currentState.position());
        gtsam::Pose3 lidarPose = imuPose.compose(imu2Lidar); // imuPose在lidar坐标系下的pose

        odometry.pose.pose.position.x = lidarPose.translation().x();
        odometry.pose.pose.position.y = lidarPose.translation().y();
        odometry.pose.pose.position.z = lidarPose.translation().z();
        odometry.pose.pose.orientation.x = lidarPose.rotation().toQuaternion().x();
        odometry.pose.pose.orientation.y = lidarPose.rotation().toQuaternion().y();
        odometry.pose.pose.orientation.z = lidarPose.rotation().toQuaternion().z();
        odometry.pose.pose.orientation.w = lidarPose.rotation().toQuaternion().w();
        
        odometry.twist.twist.linear.x = currentState.velocity().x();
        odometry.twist.twist.linear.y = currentState.velocity().y();
        odometry.twist.twist.linear.z = currentState.velocity().z();
        odometry.twist.twist.angular.x = thisImu.angular_velocity.x + prevBiasOdom.gyroscope().x();
        odometry.twist.twist.angular.y = thisImu.angular_velocity.y + prevBiasOdom.gyroscope().y();
        odometry.twist.twist.angular.z = thisImu.angular_velocity.z + prevBiasOdom.gyroscope().z();
        // information for VINS initialization
        odometry.pose.covariance[0] = double(imuPreintegrationResetId);
        odometry.pose.covariance[1] = prevBiasOdom.accelerometer().x();
        odometry.pose.covariance[2] = prevBiasOdom.accelerometer().y();
        odometry.pose.covariance[3] = prevBiasOdom.accelerometer().z();
        odometry.pose.covariance[4] = prevBiasOdom.gyroscope().x();
        odometry.pose.covariance[5] = prevBiasOdom.gyroscope().y();
        odometry.pose.covariance[6] = prevBiasOdom.gyroscope().z();
        odometry.pose.covariance[7] = imuGravity;
        pubImuOdometry.publish(odometry);

        // publish imu path
        static nav_msgs::Path imuPath;
        static double last_path_time = -1;
        if (imuTime - last_path_time > 0.1)
        {
            last_path_time = imuTime;
            geometry_msgs::PoseStamped pose_stamped;
            pose_stamped.header.stamp = thisImu.header.stamp;
            pose_stamped.header.frame_id = "odom";
            pose_stamped.pose = odometry.pose.pose;
            imuPath.poses.push_back(pose_stamped);
            while(!imuPath.poses.empty() && abs(imuPath.poses.front().header.stamp.toSec() - imuPath.poses.back().header.stamp.toSec()) > 3.0)
                imuPath.poses.erase(imuPath.poses.begin());
            if (pubImuPath.getNumSubscribers() != 0)
            {
                imuPath.header.stamp = thisImu.header.stamp;
                imuPath.header.frame_id = "odom";
                pubImuPath.publish(imuPath);
            }
        }

        // publish transformation
        tf::Transform tCur;
        tf::poseMsgToTF(odometry.pose.pose, tCur);
        tf::StampedTransform odom_2_baselink = tf::StampedTransform(tCur, thisImu.header.stamp, "odom", "base_link");
        tfOdom2BaseLink.sendTransform(odom_2_baselink);
    }
};


int main(int argc, char** argv)
{
    ros::init(argc, argv, "lidar");
    
    IMUPreintegration ImuP;

    ROS_INFO("\033[1;32m----> Lidar IMU Preintegration Started.\033[0m");

    ros::spin();
    
    return 0;
}
