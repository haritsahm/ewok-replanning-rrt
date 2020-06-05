#ifndef RRTSTAR3D_H
#define RRTSTAR3D_H

#include <ewok/ed_ring_buffer.h>
#include <ewok/polynomial_trajectory_3d.h>
#include <ewok/uniform_bspline_3d.h>
#include <ros/console.h>
#include <ros/ros.h>
#include <std_msgs/ColorRGBA.h>
#include <visualization_msgs/Marker.h>
#include <visualization_msgs/MarkerArray.h>
#include <eigen_conversions/eigen_msg.h>
#include <Eigen/Eigen>
#include <algorithm>
#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <queue>
#include <random>
#include <vector>
#include <std_msgs/ColorRGBA.h>

namespace ewok
{
template <int _N, typename _Scalar = double, typename _Datatype = int16_t>
class RRTStar3D
{
public:
  typedef Eigen::Matrix<_Scalar, 3, 1> Vector3;
  typedef Eigen::Matrix<_Scalar, 3, 3> Matrix3;
  typedef Eigen::Matrix<_Scalar, Eigen::Dynamic, Eigen::Dynamic> MatrixX;
  typedef Eigen::Transform<_Scalar, 3, Eigen::Affine> Affine3;
  typedef Eigen::Quaternion<_Scalar> Quaternion;

  typedef Eigen::Matrix<int, 3, 1> Vector3i;
  typedef std::tuple<Vector3, Vector3, bool> Edge;
  typedef std::pair<Vector3, Vector3> PPoint;
  typedef std::pair<Vector3, bool> PointBool;
  typedef std::pair<Eigen::Vector3f, bool> PointBoolF;

  typedef std::shared_ptr<RRTStar3D<_N, _Scalar>> Ptr;

  struct Node
  {
    std::vector<Node*> children_;
    Node* parent_ = NULL;
    Vector3 pos_;
    _Scalar cost_;
  };

  RRTStar3D(_Scalar step_size = 0.5, _Scalar rrt_factor = 1.1, _Scalar radius = 1, _Scalar solve_tmax = 1,
            _Scalar dt = 0.5)
    : spline_(dt)
    , step_size_(step_size)
    , rrt_factor_(rrt_factor)
    , radius_(radius)
    , debugging_(true)
    , flat_height(true)
    , sampling_alpha(0.2)
    , sampling_beta(0.5)
    , max_solve_t_(solve_tmax)
    , dt_(dt)
    , flag_sol_found(false)
    , flag_rrt_running(false)
    , algorithm_(false)
  {

    current_t = 0;

    flag_rrt_started = flag_rrt_finished = false;
    flag_not_enough = flag_force_stopped = false;
    flag_start_found = flag_stop_found = false;
    traj_point_counter = _N;
    flag_hold_dt = false;
    flag_vizualize_output = false;
    time_stamped = false;
    flag_rewire_root = false;
    flag_temp_inserted = false;
    flag_new_path_selected = false;
  }

  void reset()
  {
    for (auto p : nodes_)
    {
      delete p;
    }
    std::cout << "nodes Clear" << std::endl;
    nodes_.clear();

    //    for (auto p : x_sol_)
    //    {
    //      delete p;
    //    }
    std::cout << "x_sol Clear" << std::endl;

    x_sol_.clear();

    //    for (auto p : solution_queue)
    //    {
    //      delete p;
    //    }
    std::cout << "solution_queue Clear" << std::endl;

    solution_queue.clear();

    path_point_.clear();
    edges_.clear();

    solution_points.clear();

    std::cout << "Others Clear" << std::endl;


  }

  void initialize()
  {
    for (auto p : nodes_)
    {
      delete p;
    }
    nodes_.clear();

    for (auto p : x_sol_)
    {
      delete p;
    }
    x_sol_.clear();

    edges_.clear();

    root_ = new Node;
    root_->parent_ = NULL;
    root_->pos_ = start_;
    root_->cost_ = 0;
    lastNode_ = root_;
    nodes_.push_back(root_);

    last_solution = new Node;
    sub_root = new Node;
    sub_root = root_;

    goal_node = new Node;
  }

  void setRobotPos(const Vector3& pos)
  {
    robot_pos = pos;
  }

  void setRobotPose(const Affine3& m)
  {
    robot_pose_ = m;
  }

  void setPolynomialTrajectory(typename ewok::PolynomialTrajectory3D<10, _Scalar>::Ptr& trajectory)
  {
    trajectory_ = trajectory;
  }

  void setDistanceBuffer(typename ewok::EuclideanDistanceRingBuffer<_N, _Datatype, _Scalar>::Ptr& edrb)
  {
    edrb_ = edrb;
  }


  void addControlPoint(const Vector3& point, int num = 1)
  {
    for (int i = 0; i < num; i++)
    {
      spline_.push_back(point);
      traj_points.push_back(point);
    }
  }

  UniformBSpline3D<_N, _Scalar> getSpline()
  {
    return spline_;
  }

  void setStartPoint(Vector3 start)
  {
    start_ = start;
  }

  bool getNextPt(Vector3 & next_point)
  {
    if(traj_point_counter == 0)
    {
      Vector3 point = traj_points[traj_point_counter];
      last_point = point;
      next_point = point;
      return true;
    }

    else if(Vector3(robot_pose_.translation() - last_point).norm() < 1)
    {
      Vector3 point = traj_points[traj_point_counter];
      if (traj_point_counter < traj_points.size()-1)
        traj_point_counter++;
      last_point = point;
      next_point = point;
      return true;
    }
    else {
      return false;
    }
  }

  void clearRRT()
  {
    // flag_rrt_started = false;
    // flag_rrt_finished = false;
    flag_vizualize_output = false;
    reset();
  }

  bool RRTVisualize()
  {
    return flag_vizualize_output;
  }

  bool isRunning()
  {
    return flag_rrt_running;
  }

  void setTargetPoint(Vector3 target)
  {
    target_ = target;
    goal_node->pos_ = target;
    global_min_cost = distance(start_, target_);

    // informed rrt sampling
    {
      Vector3 id1 = Matrix3::Identity().col(0);

      Vector3 a_1 = (target_ - start_) / Vector3(target_ - start_).norm();
      Matrix3 M = a_1 * id1.transpose();

      Eigen::JacobiSVD<MatrixX> svd(M, Eigen::ComputeThinU | Eigen::ComputeThinV);

      Eigen::DiagonalMatrix<_Scalar, 3> diag(1, 1,
                                             svd.matrixU().determinant() * svd.matrixV().transpose().determinant());

      C_rotation = svd.matrixU() * diag * svd.matrixV();
    }
  }

  void setHeight(Vector3 point, bool status = true)
  {
    last_point = point;
    flat_height = status;
    height_ = point;
  }

  _Scalar getStepSize()
  {
    return step_size_;
  }

  _Scalar getCost(Node* n)
  {
    return n->cost_;
  }

  _Scalar getDistCost(Node* p, Node* q)
  {
    return distance(q->pos_, p->pos_);
  }

  _Scalar distance(Vector3& p1, Vector3& p2)
  {
    return (p2 - p1).norm();
  }

  bool isNear(Vector3& point, _Scalar tol = 2)
  {
    if (distance(point, target_) < tol)
      return true;
    return false;
  }

  bool isCollision(Node* p, Node* q)
  {
    mutex.lock();
    if (!edrb_.get())
    {
      ROS_INFO_COND_NAMED(debugging_, "RRT PLANNER", "EDRB ERROR");
      return true;
    }
    bool collision = false;

    std::vector<Vector3> point_check;

    Vector3 len = (q->pos_ - p->pos_);
    point_check.push_back(p->pos_ + 0 * len);
    point_check.push_back(p->pos_ + 0.5 * len);
    point_check.push_back(p->pos_ + 1 * len);

    for (Vector3 pt : point_check)
    {
      collision = edrb_->isNearObstacle(pt, radius_);
      if (collision)
        break;
    }

    mutex.unlock();

    return collision;
  }

  bool isCollision(Vector3 from, Vector3 to)
  {
    mutex.lock();
    if (!edrb_.get())
    {
      ROS_INFO_COND_NAMED(debugging_, "RRT PLANNER", "EDRB ERROR");
      return true;
    }
    bool collision = false;

    std::vector<Vector3> point_check;

    Vector3 len = to-from;
    point_check.push_back(from+ 0 * len);
    point_check.push_back(from + 0.5 * len);
    point_check.push_back(from + 1 * len);

    for (Vector3 pt : point_check)
    {
      collision = edrb_->isNearObstacle(pt, radius_);
      if (collision)
        break;
    }

    mutex.unlock();

    return collision;
  }

  _Scalar getRandomNumber(_Scalar a, _Scalar b)
  {
    std::random_device rseed;
    std::mt19937 rng(rseed());
    std::uniform_real_distribution<_Scalar> dist(a, b);
    _Scalar num = dist(rng);

    return num;
  }

  _Scalar getbestCost()
  {
    _Scalar min_cost = std::numeric_limits<_Scalar>::infinity();
    _Scalar min_dist = 3*step_size_;
    for (auto n : x_sol_)
    {
      if (n->cost_ <= min_cost && distance(n->pos_, target_) < min_dist)
      {
        min_cost = n->cost_;
        min_dist = distance(n->pos_, target_);
      }
    }

    return min_cost;
  }

  Node* findSolutionNode()
  {
    Node* final = new Node;
    _Scalar min_cost = std::numeric_limits<_Scalar>::infinity();
    _Scalar min_dist = std::numeric_limits<_Scalar>::infinity();
    for (auto n : x_sol_)
    {
      if (n->cost_ <= min_cost && distance(n->pos_, target_) < min_dist)
      {
        min_cost = n->cost_;
        min_dist = distance(n->pos_, target_);
        final = n;
      }
    }
    std::cout << "pos final" << final->pos_.transpose() << std::endl;
    return final;
  }

  void removeNodeinList(Node* target_node, std::list<Node*>& nodes)
  {
    typename std::list<Node*>::iterator it;
    for ( it = nodes.begin(); it != nodes.end(); ) {
      if( it == target_node) {
        delete * it;
        it = nodes.erase(it);
      }
      else {
        ++it;
      }
    }
  }

  void removeNodeinVector(Node* target_node, std::vector<Node*>& nodes)
  {
    typename std::vector<Node*>::iterator it;
    for ( it = nodes.begin(); it != nodes.end(); ) {
      if( (*it) == target_node) {
        delete it;
        it = nodes.erase(it);
      }
      else {
        ++it;
      }
    }
  }

  Vector3 BallSampling()
  {
    _Scalar theta = 2 * M_PI * getRandomNumber(0, 1);
    _Scalar phi = acos(1 - 2 * getRandomNumber(0, 1));
    _Scalar x = sin(phi) * cos(theta);
    _Scalar y = sin(phi) * sin(theta);
    _Scalar z = cos(phi);

    return Vector3(x, y, z);
    ;
  }

  Vector3 LineSampling()
  {
    Node* near_n = getNearestNode(goal_node);
    Vector3 len = goal_node->pos_ - near_n->pos_;
    len = len / len.norm();

    Vector3 point = near_n->pos_ + len * step_size_;
    Vector3i point_idx;
    edrb_->getIdxBuffer(point, point_idx);
    return point;
  }

  Vector3 UniformSampling()
  {
    Vector3 point_min, point_max, rand_point;
    Vector3i point_idx, center_idx;
    edrb_->getVolumeMinMax(point_min, point_max);
    center_idx = edrb_->getVolumeCenter();

    do
    {
      if (flat_height)
        rand_point = Vector3(getRandomNumber(point_min.x(), point_max.x()),
                             getRandomNumber(point_min.y(), point_max.y()), height_.z());
      else
      {
        rand_point = Vector3(getRandomNumber(point_min.x(), point_max.x()),
                             getRandomNumber(point_min.y(), point_max.y()), getRandomNumber(0, point_max.z()));
      }
      edrb_->getIdxBuffer(rand_point, point_idx);

    } while (edrb_->isOccupied(point_idx));

    return rand_point;
  }

  Vector3 EllipsoidSampling(_Scalar c_max)
  {
    Vector3 point_min, point_max;
    Vector3i point_idx;
    edrb_->getVolumeMinMax(point_min, point_max);
    Vector3 pos;
    if (!isinf(c_max))
    {
      _Scalar c_min = Vector3(target_-start_).norm();
      if (c_max < c_min)
        c_max = c_min;

      Vector3 x_center = (start_ + target_) / 2;
      _Scalar r_2 = sqrt(pow(c_max, 2) - pow(c_min, 2)) / 2;
      Eigen::DiagonalMatrix<_Scalar, 3> L((c_max / 2), r_2, r_2);

      do
      {
        Vector3 x_ball = BallSampling();
        pos = C_rotation * L * x_ball + x_center;


        if (flat_height)
          pos.z() = height_.z();
        edrb_->getIdxBuffer(pos, point_idx);

      } while (edrb_->isOccupied(point_idx));

      Vector3 x_ball = BallSampling();
      pos = C_rotation * L * x_ball + x_center;


      if (flat_height)
        pos.z() = height_.z();

      //      if(pos.x() < point_min.x()) pos.x() = point_min.x();
      //      if(pos.y() < point_min.y()) pos.y() = point_min.y();
      //      if(pos.x() > point_max.x()) pos.x() = point_max.x();
      //      if(pos.y() > point_max.y()) pos.y() = point_max.y();
    }
    else
    {
      pos = UniformSampling();
    }

    return pos;
  }

  Node* randomSampling(_Scalar& c_max)
  {
    Vector3 pos;
    if (isinf(c_max))
    {
      _Scalar P_r = getRandomNumber(0, 1);

      if (P_r > 1 - sampling_alpha)
      {
        ROS_INFO_COND_NAMED(debugging_, "RRT PLANNER", "Line Sampling");
        pos = LineSampling();
      }

      else if (P_r <= 1 - (sampling_alpha / sampling_beta))
      {
        ROS_INFO_COND_NAMED(debugging_, "RRT PLANNER", "Uniform Sampling");
        pos = UniformSampling();
      }

      else
      {
        ROS_INFO_COND_NAMED(debugging_, "RRT PLANNER", "Ellipsoid Sampling");
        pos = EllipsoidSampling(c_max);
      }
    }
    else
    {
      ROS_INFO_COND_NAMED(debugging_, "RRT PLANNER", "Ellipsoid Sampling");
      pos = EllipsoidSampling(c_max);
    }

    Node* rand_node = new Node;
    rand_node->pos_ = pos;
    return rand_node;
  }

  void InsertNode(Node* min_node, Node* new_node)
  {
    new_node->parent_ = min_node;
    new_node->cost_ = min_node->cost_ + getDistCost(min_node, new_node);
    min_node->children_.push_back(new_node);
    edges_.push_back(std::make_tuple(min_node->pos_, new_node->pos_, false));
    nodes_.push_back(new_node);
    lastNode_ = new_node;
  }

  Node* getNearestNodeDistance(Node* node_, _Scalar max_distance = 1)
  {
    _Scalar minCost = 0;
    _Scalar minDist = distance(node_->pos_, target_);
    Node* closest = new Node;

    for (auto x_near: nodes_)
    {
      _Scalar dist2origin = distance(node_->pos_, x_near->pos_);
      _Scalar dist2target = 3*distance(x_near->pos_, target_);
      _Scalar cost = getCost(x_near);
      if (dist2origin <= max_distance/* && dist2target <= minDist*/ && cost >= minCost)
      {
        minDist = dist2target;
        minCost = cost;
        closest = x_near;
      }
    }

    return closest;
  }

  Node* getNearestNode(Node* node_)
  {
    _Scalar minDist = std::numeric_limits<_Scalar>::infinity();
    Node* closest = NULL;

    for (auto x_near: nodes_)
    {
      _Scalar dist = distance(node_->pos_, x_near->pos_);
      if (dist < minDist)
      {
        minDist = dist;
        closest = x_near;
      }
    }
    return closest;
  }

  void getNearestNodes(Node* node, _Scalar radius, std::vector<Node*>& near)
  {
    for (auto n : nodes_)
    {
      _Scalar dist = distance(node->pos_, n->pos_);
      if (dist < radius)
        near.push_back(n);
    }
  }

  Node* getConfigurationNode(Node* rand_, Node* nearest_)
  {
    Node* node_ = new Node;
    Vector3 rand_pos = rand_->pos_;
    Vector3 near_pos = nearest_->pos_;
    Vector3 midPos = rand_pos - near_pos;
    if (midPos.norm() > step_size_)
    {
      midPos = midPos / midPos.norm();
      node_->pos_ = near_pos + step_size_ * midPos;
    }
    else
    {
      node_->pos_ = rand_pos;
    }

    return node_;
  }

  Node* chooseParent(Node* nearest_node, Node* new_node, std::vector<Node*> near_nodes)
  {
    Node* min_node = nearest_node;
    _Scalar min_cost = getCost(nearest_node) + getDistCost(nearest_node, new_node);
    for (auto p : near_nodes)
    {
      _Scalar new_cost = getCost(p) + getDistCost(p, new_node);
      if (!isCollision(p, new_node) && new_cost < min_cost)
      {
        min_node = p;
        min_cost = new_cost;
      }
    }

    return min_node;
  }

  void reWireTree(Node* min_node, Node* new_node, std::vector<Node*> near_nodes)
  {
    for (Node* x_near : near_nodes)
    {
      _Scalar cost_old = x_near->cost_;
      _Scalar cost_new = getCost(new_node) + getDistCost(new_node, x_near);
      if (!isCollision(new_node, x_near) && cost_new < cost_old)
      {
        Node* x_parent = x_near->parent_;
        x_parent->children_.erase(std::remove(x_parent->children_.begin(), x_parent->children_.end(), x_near),
                                  x_parent->children_.end());
        edges_.erase(std::remove(edges_.begin(), edges_.end(), std::make_tuple(x_parent->pos_, x_near->pos_, false)),
                     edges_.end());

        x_near->cost_ = cost_new;
        x_near->parent_ = new_node;
        new_node->children_.push_back(x_near);
        edges_.push_back(std::make_tuple(new_node->pos_, x_near->pos_, false));
      }
    }
  }

  bool solutionFound()
  {
    return flag_sol_found;
  }

  void process()
  {
    if (current_t < trajectory_->duration())
    {
      std::vector<Vector3> traj_pts = trajectory_->evaluates(current_t, dt_, 4, 0);
      end_segment_point = trajectory_->evaluateEndSegment(current_t, 0);

      if (Vector3(robot_pos - traj_pts.front()).norm() > 2.5)
        flag_hold_dt = true;
      else
        flag_hold_dt = false;

      // need to update from subroot
      if(flag_rrt_running)
      {
        if(flag_sol_found && Vector3(robot_pose_.translation() - sub_root->pos_).norm() < 2*step_size_)
        {
          ROS_WARN_STREAM_COND_NAMED(algorithm_, "Proces RRT 2", "Add Root to Path");
          if (std::find(traj_points.begin(), traj_points.end(), sub_root->pos_) == traj_points.end())
          {
            Vector3 last_point = traj_points[traj_points.size()-1];

            Vector3 mid_point = (last_point + sub_root->pos_)/2;

            traj_points.push_back(mid_point);
            spline_.push_back(mid_point);
            traj_points.push_back(sub_root->pos_);
            spline_.push_back(sub_root->pos_);
          }
          flag_rewire_root = true;
        }
        else if(flag_new_path_selected)
        {
          flag_new_path_selected = false;
          Vector3 last_point = traj_points[traj_points.size()-1];

          Vector3 mid_point = (last_point + sub_root->pos_)/2;

          traj_points.push_back(mid_point); traj_points.push_back(sub_root->pos_);
          spline_.push_back(mid_point); spline_.push_back(sub_root->pos_);
        }
      }

      else if(!flag_rrt_running && flag_rrt_finished)
      {
        ROS_WARN_STREAM_COND_NAMED(algorithm_, "Proces RRT 2", "RRT FINISHED");
        std::cout << "RRT FINISHED START DELETING" << std::endl;
        //        if (std::find(traj_points.begin(), traj_points.end(), sub_root->pos_) == traj_points.end())
        //        {
        Vector3 last_point = traj_points[traj_points.size()-1];
        Vector3 mid_point = (last_point + target_)/2;
        traj_points.push_back(mid_point); traj_points.push_back(target_);
        spline_.push_back(mid_point); spline_.push_back(target_);
        //        }
        flag_rewire_root = false;
        flag_rrt_finished = false;
        flag_stop_found = false;
        flag_rrt_started = false;
        current_t = reset_dt_;
        reset();
        delete tra_gene_thread_;
      }


      if (edrb_->insideVolume(traj_pts))
      {
        std::vector<PointBool> traj_pts_bool = edrb_->isNearObstacle2(traj_pts, radius_ + 0.1);

        if ((!flag_stop_found && flag_rrt_started))  // End point search
        {
          ROS_WARN_COND_NAMED(algorithm_, "Process", "End Point Search");

          for (int i = 0; i < traj_pts_bool.size() - 1; i++)
          {
            PointBool prev_pt = traj_pts_bool[i];
            PointBool next_pt = traj_pts_bool[i + 1];

            /*
             * To prevent multiple points inside path_checker
             */
            if (std::find(path_checker.begin(), path_checker.end(), prev_pt) == path_checker.end())
            {
              path_checker.push_back(prev_pt);
            }

            if (std::find(path_checker.begin(), path_checker.end(), next_pt) == path_checker.end())
            {
              path_checker.push_back(next_pt);
            }

            // If multiple point is blocked, insert them to the list
            if (prev_pt.second && next_pt.second)
            {
              if (std::find(obs_list.begin(), obs_list.end(), prev_pt.first) == obs_list.end())
                obs_list.push_back(prev_pt.first);
              if (std::find(obs_list.begin(), obs_list.end(), next_pt.first) == obs_list.end())
                obs_list.push_back(next_pt.first);
            }

            // else if the second point is free, set as real target point
            else if (prev_pt.second && !next_pt.second)
            {
              // less than counter
              if (obs_list.size() < 3)
              {
                ROS_WARN_COND_NAMED(algorithm_, "Process", "Less Counter - Skipping");
                flag_not_enough = true;

                obs_list.clear();
                break;
              }

              // normal
              else
              {
                ROS_WARN_COND_NAMED(algorithm_, "Process", "Found Normal Endpoint");
                solving_queue.push_back(std::make_pair(curr_start_pt, next_pt.first));
                ROS_WARN_STREAM_COND_NAMED(algorithm_, "Proces",
                                           "Starting:" << curr_start_pt.transpose()
                                           << " Endpoint: " << next_pt.first.transpose());
                target_ = next_pt.first;
                setTargetPoint(next_pt.first);

                obstacle_counter = 0;
                flag_real_target = true;
                flag_stop_found = true;
                flag_not_enough = false;
                reset_dt_ = current_t + dt_ * (i-1);
                break;
              }
            }
          }
        }

        else if (!flag_rrt_started && !flag_stop_found)  // found starting point
        {
          ROS_WARN_COND_NAMED(algorithm_, "Process", "Start Point Search");

          for (int i = 0; i < traj_pts_bool.size() - 1; i++)  // Start point search
          {
            PointBool prev_pt = traj_pts_bool[i];
            PointBool next_pt = traj_pts_bool[i + 1];

            /*
             * If free, insert prev_pt to traj_point and spline
             */
            if (!prev_pt.second)
            {
              if (std::find(traj_points.begin(), traj_points.end(), prev_pt.first) == traj_points.end())
              {
                traj_points.push_back(prev_pt.first);
                spline_.push_back(prev_pt.first);
              }
            }

            /*
             * To prevent multiple points inside path_checker
             */
            if (std::find(path_checker.begin(), path_checker.end(), prev_pt) == path_checker.end())
            {
              path_checker.push_back(prev_pt);
            }

            if (std::find(path_checker.begin(), path_checker.end(), next_pt) == path_checker.end())
            {
              path_checker.push_back(next_pt);
            }

            /*
             * If the next_pt is not free, set as rrt starting point
             * and use the end of the segment point as target
             */
            if (!prev_pt.second && next_pt.second)
            {

              start_ = prev_pt.first;
              target_ = end_segment_point;

              initialize();
              setTargetPoint(end_segment_point);

              ROS_WARN_STREAM_COND_NAMED(algorithm_, "Proces RRT 2",
                                         "Proces RRT 2 Starting:" << start_.transpose() << " Endpoint: " << target_.transpose());

              std::cout << "Create Thread " << std::endl;
              tra_gene_thread_ = new boost::thread(boost::bind(&RRTStar3D::solveRRT, this));

              for(int i=0; i < 5; i++)
              {
                traj_points.push_back(start_);
                spline_.push_back(start_);
              }
              curr_start_pt = prev_pt.first;
              obs_list.push_back(next_pt.first);
              obstacle_counter = 1;
              flag_start_found = true;
              flag_real_target = false;
              break;
            }
          }
        }

        if ((!flag_hold_dt && !flag_rrt_running)|| (flag_rrt_running && !flag_stop_found))
          current_t += dt_;
      }
    }
  }

  void solveRRT()
  {
    flag_rrt_running = true;
    flag_rrt_started = true;
    flag_rrt_finished = false;
    Node* final = NULL;
    solution_node = new Node;
    int counter = 0;
    bool found = false;
    flag_sol_found = false;
    path_point_.clear();

    time_stamped = true;
    std::chrono::high_resolution_clock::time_point rrt_stamp = std::chrono::high_resolution_clock::now();
    search_t_stamp = std::chrono::high_resolution_clock::now();
    ROS_INFO_COND_NAMED(algorithm_, "RRT PLANNER", "Starting RRT");
    best_cost_ = std::numeric_limits<_Scalar>::infinity();
    int counter_limit = 100;
    while (Vector3(sub_root->pos_ - goal_node->pos_).norm() > 2*step_size_ ||
           ( Vector3(robot_pose_.translation() - goal_node->pos_).norm() > 2*step_size_))
    {
      auto it_sol=find(solution_queue.begin(),solution_queue.end(),sub_root);
      int sol_pos = it_sol - solution_queue.begin();

      if(Vector3(sub_root->pos_ - target_).norm() < step_size_*rrt_factor_ && !isCollision(sub_root, goal_node))
        break;

      else if(Vector3(sub_root->pos_ - target_).norm() < step_size_*2 && sol_pos > solution_queue.size()-3 && !isCollision(sub_root, goal_node)) break;


      // Too Short
      if (flag_not_enough)
      {
        // clear
        flag_not_enough = false;
        flag_rrt_started = false;
        flag_force_stopped = true;
        break;
      }

      // cusing invalid pointer
      if(flag_rewire_root && found && solution_queue.size()>0)
      {
        bool replaced = false;
        for(int i = 0; i < solution_queue.size()-1; i++)
        {

          if(solution_queue[i]==sub_root)
          {
            //            solution_queue[i+1]->children_.push_back(root_);
            //            solution_queue[i+1]->parent_ = NULL;
            //            solution_queue[i+1]->cost_ = 0;

            //            root_->parent_ = solution_queue[i+1];
            //            root_->cost_ = getDistCost(solution_queue[i+1], root_);
            //            root_ = solution_queue[i+1];
            sub_root = solution_queue[i+1];
            //            sub_root->cost_ = 0;
            //            sub_root->pos_ = solution_queue[i+1]->pos_;
            replaced = true;

            //            setStartPoint(sub_root->pos_);
            setTargetPoint(target_);
            break;
          }
        }

        if(!replaced)
        {
          _Scalar min_dist = std::numeric_limits<_Scalar>::infinity();
          Node* nearest_node = new Node;
          for(auto node: solution_queue)
          {
            if(distance(sub_root->pos_, node->pos_) < min_dist)
            {
              nearest_node = node;
              min_dist = distance(sub_root->pos_, node->pos_);
            }
          }
          flag_new_path_selected = true;
          sub_root = nearest_node;
          //          sub_root->cost_ = 0;
          //          setStartPoint(sub_root->pos_);
          setTargetPoint(target_);
        }

        flag_rewire_root = false;
      }


      ROS_INFO_COND_NAMED(debugging_, "RRT PLANNER", "Getting Random Node");
      mutex.lock();
      if (x_sol_.size() > 0)
        best_cost_ = getbestCost();
      std::cout << "might get error here" << std::endl;
      Node* rand_node = randomSampling(best_cost_);
      mutex.unlock();
      if (rand_node)
      {
        ROS_INFO_COND_NAMED(debugging_, "RRT PLANNER", "Find Nearest");
        mutex.lock();
        Node* nearest_node = getNearestNode(rand_node);

        ROS_INFO_COND_NAMED(debugging_, "RRT PLANNER", "Get Conf Node");
        Node* new_node = getConfigurationNode(rand_node, nearest_node);
        mutex.unlock();
        if (!isCollision(nearest_node, new_node))
        {
          std::vector<Node*> near_nodes;
          ROS_INFO_COND_NAMED(debugging_, "RRT PLANNER", "Get Nearest Nodes");

          //           _Scalar radius = std::min(rrt_gamma_*pow(log(nodes_.size()+1)/nodes_.size()+1, 1/2),
          //           step_size_*rrt_factor_);
          _Scalar radius = step_size_ * rrt_factor_;
          getNearestNodes(new_node, radius, near_nodes);

          ROS_INFO_COND_NAMED(debugging_, "RRT PLANNER", "Find Parent");

          Node* min_node = nearest_node;
          _Scalar min_cost = getCost(nearest_node) + getDistCost(nearest_node, new_node);
          for (auto x_near : near_nodes)
          {
            _Scalar new_cost = getCost(x_near) + getDistCost(x_near, new_node);
            if (!isCollision(x_near, new_node) && new_cost < min_cost)
            {
              min_node = x_near;
              min_cost = new_cost;
            }
          }

          new_node->parent_ = min_node;
          new_node->cost_ = min_cost;
          min_node->children_.push_back(new_node);
          edges_.push_back(std::make_tuple(min_node->pos_, new_node->pos_, false));
          nodes_.push_back(new_node);
          lastNode_ = new_node;

          ROS_INFO_COND_NAMED(debugging_, "RRT PLANNER", "Rewire Tree");
          for (Node* x_near : near_nodes)
          {
            if (!isCollision(new_node, x_near) && (getCost(new_node) + getDistCost(new_node, x_near)) < getCost(x_near))
            {
              Node* n_parent = x_near->parent_;
              n_parent->children_.erase(std::remove(n_parent->children_.begin(), n_parent->children_.end(), x_near),
                                        n_parent->children_.end());

              //              auto it_child = std::find(n_parent->children_.begin(), n_parent->children_.end(), x_near);
              //                  if (it_child != n_parent->children_.end()) { n_parent->children_.erase(it_child); }

              edges_.erase(
                    std::remove(edges_.begin(), edges_.end(), std::make_tuple(n_parent->pos_, x_near->pos_, false)),
                    edges_.end());

              //                  auto it_edge = std::find(edges_.begin(), edges_.end(), std::make_tuple(n_parent->pos_, x_near->pos_, false));
              //                      if (it_edge != edges_.end()) { edges_.erase(it_edge); }

              x_near->cost_ = getCost(new_node) + getDistCost(new_node, x_near);
              x_near->parent_ = new_node;
              new_node->children_.push_back(x_near);
              edges_.push_back(std::make_tuple(new_node->pos_, x_near->pos_, false));
            }
          }
        }
      }

      if (isNear(lastNode_->pos_, step_size_ * 3))
      {
        ROS_WARN_COND(algorithm_, "Found Solution");
        if (std::find(x_sol_.begin(), x_sol_.end(), lastNode_) == x_sol_.end())
        {
          x_sol_.push_back(lastNode_);
        }
        found = true;
        flag_sol_found = true;
        if(temp_solution && x_sol_.size() > 0)
        {
          std::cout << "removing temp goal" << std::endl;
          x_sol_.erase(std::remove(x_sol_.begin(), x_sol_.end(), temp_solution), x_sol_.end());
          //temp_solution = NULL;
        }
      }

      //generate solution in time
      std::chrono::duration<double> t_elapsed = std::chrono::high_resolution_clock::now() - search_t_stamp;
      if(t_elapsed.count() > max_solve_t_ || Vector3(robot_pose_.translation() - sub_root->pos_).norm() < 2*step_size_)
      {
        if(found)
        {
          std::cout << "Getting Solution Path" << std::endl;
          std::vector<Node*> temp_solution;
          Node* possible_solution = new Node;
          possible_solution= findSolutionNode();
          final = possible_solution;

          // if empty, add new solution to path
          if(solution_queue.empty())
          {
            solution_node = possible_solution;
            //            last_solution = possible_solution;
            final = possible_solution;
            std::cout << "Clearing memory" << std::endl;

            path_point_.clear();
            while (final != NULL)
            {
              Vector3 pos = final->pos_;
              path_point_.push_front(pos);
              solution_queue.insert(solution_queue.begin(), final);
              final = final->parent_;
            }
            path_point_.push_back(target_);

            std::cout << "Found new Solution" << std::endl;
          }
          else
          {
            std::cout << "Checking" << std::endl;
            // check and combine path with original solution
            while (final != NULL)
            {
              temp_solution.insert(temp_solution.begin(), final);
              final = final->parent_;
            }
            std::cout << "Checking Solution" << std::endl;

            auto it_sol=find(solution_queue.begin(),solution_queue.end(),sub_root);
            int sol_pos = it_sol - solution_queue.begin();

            std::cout << "Checking Suggested" << std::endl;

            auto it_temp=find(temp_solution.begin(),temp_solution.end(), sub_root);
            int temp_pos = it_temp - temp_solution.begin();

            std::cout << "Trying to Replace" << std::endl;
            if(temp_pos != 0)
            {
              if(it_temp != temp_solution.end())
              {
                //                solution_queue.erase(solution_queue.begin()+sol_pos,solution_queue.end());
                //                solution_queue.insert(solution_queue.end(), temp_solution.begin()+temp_pos, temp_solution.end());
                std::cout << "Replace with new" << std::endl;
                solution_queue.erase(solution_queue.begin(), solution_queue.end());
                solution_queue.clear();
                solution_queue = temp_solution;
                final = sub_root;
                while (final != NULL)
                {
                  Vector3 pos = final->pos_;
                  path_point_.push_front(pos);
                  final = final->parent_;
                }
                path_point_.push_back(target_);
              }

              // if there's no match or new path with smaller cost
              // Need to rewrte the new solution method
              else if(it_temp == temp_solution.end() && possible_solution->cost_ < solution_node->cost_)
              {
                std::cout << "rewire new path"  << std::endl;
                bool close_path = false;
                Node* close_node = new Node;
                for(auto p: temp_solution)
                {
                  if(distance(p->pos_, sub_root->pos_) < step_size_*rrt_factor_)
                  {
                    close_node = p;
                    close_path = true;
                  }

                }

                solution_queue.erase(solution_queue.begin(), solution_queue.end());
                solution_queue.clear();
                std::cout << "No nearest path to sub_root" << std::endl;
                std::cout << "Cleared solution queue" << std::endl;
                solution_queue = temp_solution;
                final = possible_solution;
                //                sub_root = close_node;
                while (final != NULL)
                {
                  Vector3 pos = final->pos_;
                  path_point_.push_front(pos);
                  final = final->parent_;
                }
                path_point_.push_back(target_);
                std::cout << "Found new Solution" << std::endl;
              }

              //                if(close_path)
              //                {
              //                  std::cout << "Found new Path from nearest sub_root" << std::endl;
              ////                  close_node->parent_ = sub_root;
              ////                  close_node->cost_ = getCost(sub_root) + distance(sub_root->pos_, close_node->pos_);
              ////                  sub_root->children_.push_back(close_node);
              //                  auto it_temp=find(temp_solution.begin(),temp_solution.end(), close_node);
              //                  int temp_pos = it_temp - temp_solution.begin();
              //                  solution_queue.erase(solution_queue.begin()+sol_pos,solution_queue.end());
              //                  solution_queue.insert(solution_queue.end(), temp_solution.begin()+temp_pos, temp_solution.end());

              ////                  solution_queue.erase(solution_queue.begin(), solution_queue.end());
              ////                  solution_queue.clear();
              //                  close_node->parent_ = sub_root;
              //                  close_node->cost_ = getCost(sub_root) + distance(sub_root->pos_, close_node->pos_);
              //                  sub_root->children_.push_back(close_node);
              //                  std::cout << "Cleared solution queue" << std::endl;
              //                  std::cout << "root : " << root_->pos_.transpose() << " | goal : " << target_.transpose() << " | subroot : " << sub_root->pos_.transpose() << std::endl;
              //                  std::cout << "solution queue" << std::endl;
              //                  for(auto node:solution_queue)
              //                  {
              //                    std::cout << "Node : " << node->pos_.transpose() << " | Parent: " << node->parent_->pos_.transpose() << std::endl;
              //                  }
              //                  final = solution_queue.back();
              //                  while (final != NULL)
              //                  {
              //                    Vector3 pos = final->pos_;
              //                    path_point_.push_front(pos);
              ////                    solution_queue.insert(solution_queue.begin(), final);
              //                    final = final->parent_;
              //                  }
              //                  path_point_.push_back(target_);
              //                  std::cout << "Found new Solution" << std::endl;
              //                }
              //                else
              //                {
              //                  solution_queue.erase(solution_queue.begin(), solution_queue.end());
              //                  solution_queue.clear();
              //                  std::cout << "No nearest path to sub_root" << std::endl;
              //                  std::cout << "Cleared solution queue" << std::endl;
              //                  final = possible_solution;
              //                  while (final != NULL)
              //                  {
              //                    Vector3 pos = final->pos_;
              //                    path_point_.push_front(pos);
              //                    solution_queue.insert(solution_queue.begin(), final);
              //                    final = final->parent_;
              //                  }
              //                  path_point_.push_back(target_);
              //                  std::cout << "Found new Solution" << std::endl;
              //                }
              //              }
            }

            path_point_.clear();
            for(int i = 0; i < solution_queue.size();i++)
            {
              path_point_.push_back(solution_queue[i]->pos_);
            }

            path_point_.push_back(target_);

          }
        }

        else {
          std::cout << "Find Temporary Solution" << std::endl;
          temp_solution = new Node;
          temp_solution = getNearestNodeDistance(sub_root, 2);
          std::cout << "add Temporary to Solution" << std::endl;

          if (std::find(x_sol_.begin(), x_sol_.end(), temp_solution) == x_sol_.end())
          {
            x_sol_.push_back(temp_solution);
          }
          found = true;
          flag_sol_found = true;
          std::cout << "found Temporary Solution" << std::endl;
        }

        flag_vizualize_output = true;
        search_t_stamp = std::chrono::high_resolution_clock::now();

      }

    }
    std::cout << "RRT FINISHED" << std::endl;
    flag_rrt_running = false;
    flag_rrt_finished = true;
  }


  void TrajectoryChecker(visualization_msgs::Marker& traj_marker, const std::string& frame = "world",
                         const std::string& ns = "trajectory_checker",
                         const Eigen::Vector3d& obs = Eigen::Vector3d(1, 0.5, 1),
                         const Eigen::Vector3d& free = Eigen::Vector3d(1, 1, 0))
  {
    traj_marker.header.frame_id = frame;
    traj_marker.ns = ns;
    traj_marker.id = 0;
    traj_marker.type = visualization_msgs::Marker::SPHERE_LIST;
    traj_marker.action = visualization_msgs::Marker::MODIFY;
    traj_marker.scale.x = 0.1;
    traj_marker.scale.y = 0.1;
    traj_marker.scale.z = 0.1;
    traj_marker.color.a = 1.0;

    std_msgs::ColorRGBA c_obs, c_free;
    c_obs.r = obs.x();
    c_obs.g = obs.y();
    c_obs.b = obs.z();
    c_obs.a = 1.0;

    c_free.r = free.x();
    c_free.g = free.y();
    c_free.b = free.z();
    c_free.a = 1.0;

    if (path_checker.size() != 0)
    {
      for (auto point : path_checker)
      {
        PointBool pb = point;
        geometry_msgs::Point p;
        p.x = pb.first.x();
        p.y = pb.first.y();
        p.z = pb.first.z();

        if (pb.second)
        {
          traj_marker.colors.push_back(c_obs);
          traj_marker.points.push_back(p);
        }

        else
        {
          traj_marker.colors.push_back(c_free);
          traj_marker.points.push_back(p);
        }
      }
    }
  }


  void getTrajectoryMarkers(visualization_msgs::MarkerArray& traj_marker,
                            const std::string& ns = "spline_opitimization_markers",
                            const Eigen::Vector3d& color1 = Eigen::Vector3d(0, 1, 0),
                            const Eigen::Vector3d& color2 = Eigen::Vector3d(0, 1, 1))
  {
    traj_marker.markers.resize(2);
    spline_.getVisualizationMarker(traj_marker.markers[0], ns, 0, color1, traj_point_counter, 2, color2);
    spline_.getControlPointsMarker(traj_marker.markers[1], ns, 1, color1, traj_point_counter, 2, color2);

  }

  void getTreeMarker(visualization_msgs::Marker& traj_marker, const std::string& ns, int id = 0,
                     const Eigen::Vector3f& color = Eigen::Vector3f(1, 1, 0), double scale = 0.01)
  {
    if (edges_.empty())
    {
      //      ROS_WARN("Edges Empty");
      return;
    }

    traj_marker.header.frame_id = "world";
    traj_marker.ns = ns;
    traj_marker.id = id;
    traj_marker.type = visualization_msgs::Marker::LINE_LIST;
    traj_marker.action = visualization_msgs::Marker::MODIFY;
    traj_marker.scale.x = scale;

    std_msgs::ColorRGBA c_free, c_obs;

    c_free.r = 0;
    c_free.g = 1.0;
    c_free.b = 0;
    c_free.a = 1.0;

    c_obs.r = 1.0;
    c_obs.g = 0.5;
    c_obs.b = 1.0;
    c_obs.a = 1.0;

    traj_marker.color = c_free;

    for (int i = 0; i < edges_.size() - 1; i++)
    {
      Vector3 p, q;
      p = std::get<0>(edges_[i]);
      q = std::get<1>(edges_[i]);

      geometry_msgs::Point p_, q_;
      p_.x = p.x();
      p_.y = p.y();
      p_.z = p.z();
      q_.x = q.x();
      q_.y = q.y();
      q_.z = q.z();

      if (std::get<2>(edges_[i]))
      {
        traj_marker.points.push_back(p_);
        traj_marker.points.push_back(q_);
      }
      else
      {
        traj_marker.points.push_back(p_);
        traj_marker.points.push_back(q_);
      }
    }
  }

  void getSolutionMarker(visualization_msgs::Marker& traj_marker, const std::string& ns, int id = 0,
                         const Eigen::Vector3f& color = Eigen::Vector3f(0, 1, 0), double scale = 0.01)
  {
    if (path_point_.empty())
    {
      //      ROS_WARN("Path Point Empty");
      return;
    }

    traj_marker.header.frame_id = "world";
    traj_marker.ns = ns;
    traj_marker.id = id;
    traj_marker.type = visualization_msgs::Marker::LINE_STRIP;
    traj_marker.action = visualization_msgs::Marker::MODIFY;
    traj_marker.scale.x = scale;
    traj_marker.color.a = 1.0;

    // cyan
    traj_marker.color.r = color(0);
    traj_marker.color.g = color(1);
    traj_marker.color.b = color(2);

    for (Vector3 n : path_point_)
    {
      geometry_msgs::Point p;
      p.x = n.x();
      p.y = n.y();
      p.z = n.z();

      traj_marker.points.push_back(p);
    }

    flag_vizualize_output = false;
  }

  void getRRTProperty(visualization_msgs::Marker& traj_marker, const std::string& ns, int id = 0,
                      const Eigen::Vector4f& color = Eigen::Vector4f(0, 1, 0, 1), double scale = 0.01)
  {
    traj_marker.header.frame_id = "world";
    traj_marker.ns = ns;
    if(id == 0) // hyperellipsoid
    {
      traj_marker.id = id;
      traj_marker.type = visualization_msgs::Marker::CYLINDER;
      traj_marker.action = visualization_msgs::Marker::MODIFY;

      traj_marker.color.r = color(0);
      traj_marker.color.g = color(1);
      traj_marker.color.b = color(2);
      traj_marker.color.a = color(3);

      Vector3 center = (start_+target_)/2;
      Quaternion orien(C_rotation);
      if(best_cost_ < global_min_cost)
        best_cost_ = global_min_cost;

      traj_marker.pose.position.x =center.x();
      traj_marker.pose.position.y =center.y();
      traj_marker.pose.position.z =center.z();
      traj_marker.pose.orientation.w = orien.w();
      traj_marker.pose.orientation.x = orien.x();
      traj_marker.pose.orientation.y = orien.y();
      traj_marker.pose.orientation.z = orien.z();

      traj_marker.scale.y = sqrt(pow(best_cost_,2) - pow(global_min_cost,2));
      traj_marker.scale.x = best_cost_;
      traj_marker.scale.z = 1;
    }

    else if(id ==1){
      traj_marker.id = id;
      traj_marker.type = visualization_msgs::Marker::SPHERE_LIST;
      traj_marker.action = visualization_msgs::Marker::MODIFY;

      // Need to add visual for sub_root

      std_msgs::ColorRGBA root_c, subroot_c;

      root_c.a = color(3);
      root_c.r = color(0);
      root_c.g = color(1);
      root_c.b = color(2);

      subroot_c.a = 0.2;
      subroot_c.r = 0;
      subroot_c.g = 0;
      subroot_c.b = 1;

      geometry_msgs::Point point;
      point.x = start_.x();
      point.y = start_.y();
      point.z = start_.z();
      traj_marker.points.push_back(point);
      traj_marker.colors.push_back(root_c);

      point.x = target_.x();
      point.y = target_.y();
      point.z = target_.z();
      traj_marker.points.push_back(point);
      traj_marker.colors.push_back(root_c);

      point.x = sub_root->pos_.x();
      point.y = sub_root->pos_.y();
      point.z = sub_root->pos_.z();
      traj_marker.points.push_back(point);
      traj_marker.colors.push_back(subroot_c);

      traj_marker.scale.x = 0.1;
      traj_marker.scale.y = 0.1;
      traj_marker.scale.z = 0.1;
    }

  }

protected:
  typename EuclideanDistanceRingBuffer<_N, _Datatype, _Scalar>::Ptr edrb_;
  typename PolynomialTrajectory3D<10, _Scalar>::Ptr trajectory_;

  UniformBSpline3D<_N, _Scalar> spline_;

  Vector3 start_, target_, height_, robot_pos;
  _Scalar step_size_;
  std::list<Node*> nodes_, x_sol_;
  Node *root_, *lastNode_, *goal_node;
  _Scalar rrt_factor_, radius_;
  std::list<Vector3> path_point_;
  std::vector<Edge> edges_;
  bool flat_height;
  bool debugging_;

  boost::thread* tra_gene_thread_;
  boost::mutex mutex;

  _Scalar sampling_alpha, sampling_beta;
  _Scalar max_solve_t_;

  _Scalar global_min_cost, global_best_cost;
  Matrix3 C_rotation;

  // V2
  std::list<PPoint> solving_queue;
  std::list<Vector3> obs_list;
  std::vector<Vector3> traj_points;
  std::list<PointBool> path_checker;
  Vector3 curr_start_pt;
  bool flag_rrt_started, flag_rrt_finished;
  bool flag_not_enough, flag_force_stopped;
  bool flag_hold_dt;
  _Scalar current_t;
  bool flag_start_found, flag_stop_found;
  bool algorithm_;
  int obstacle_counter, traj_point_counter;
  bool flag_vizualize_output;
  Vector3 end_segment_point;
  bool flag_force_endpoint, flag_real_target;
  _Scalar dt_, reset_dt_;
  _Scalar rrt_gamma_;

  _Scalar curr_heading_;
  Affine3 robot_pose_;

  // V3
  bool flag_sol_found, flag_rrt_running, flag_rewire_root;
  Node* solution_node, *temp_solution;
  std::vector<Vector3> solution_points;
  std::vector<Node*> solution_queue;
  Vector3 last_point;
  _Scalar best_cost_;
  bool flag_temp_inserted;
  Node* sub_root;
  Node* last_solution;
  bool flag_new_path_selected;

  bool time_stamped;
  std::chrono::high_resolution_clock::time_point search_t_stamp;
};

}  // namespace ewok

#endif  // RRTSTAR3D_H
