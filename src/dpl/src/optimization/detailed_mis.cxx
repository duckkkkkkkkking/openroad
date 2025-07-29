// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2021-2025, The OpenROAD Authors

////////////////////////////////////////////////////////////////////////////////
// Description:
// An implementation of maximum independent set matching to reduce either
// wirelength of displacement.
//
// The idea is to color the nodes (for wirelength).  Then, one can solve
// a matching problem to optimize for wirelength.  Nodes with the same
// color do not share nets so they can be repositioned without worsening
// wirelength.  Note that for displacement optimization, colors are not
// needed.
//
// There are likely many improvements which can be made to this code
// regarding the selection of nodes, etc.

#include "detailed_mis.h"

#include <lemon/cost_scaling.h>
#include <lemon/cycle_canceling.h>
#include <lemon/list_graph.h>
#include <lemon/network_simplex.h>
#include <lemon/preflow.h>
#include <lemon/smart_graph.h>
#include <random>

#include <algorithm>
#include <boost/tokenizer.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <queue>
#include <string>
#include <utility>
#include <vector>
#include <omp.h>

#include "detailed_manager.h"
#include "infrastructure/architecture.h"
#include "infrastructure/detailed_segment.h"
#include "infrastructure/network.h"
#include "util/color.h"
#include "util/journal.h"
#include "utl/Logger.h"

#include <sta/MinMax.hh>
#include "db_sta/dbNetwork.hh"
#include <sta/MinMax.hh> 
using utl::DPL;
#include <chrono>
static thread_local std::vector<omp_lock_t*> neighbour_hold_;
namespace dpl {

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
struct DetailedMis::Bucket
{
  void clear() { nodes_.clear(); }

  std::deque<Node*> nodes_;
  double xmin_ = 0.0;
  double xmax_ = 0.0;
  double ymin_ = 0.0;
  double ymax_ = 0.0;
  int i_ = 0;
  int j_ = 0;
  int travId_ = 0;
};

double DetailedMis::getSolveMatchTimeUsage(){return (double)this->solveMatchTimeUsage;} // hyx changed
double DetailedMis::getMaxFlowTimeUsage(){return (double)this->maxFlowTimeUsage;} // hyx changed
double DetailedMis::getMaxFlowCount(){return (double)this->maxFlowCount;} // hyx changed

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
DetailedMis::DetailedMis(Architecture* arch, Network* network,sta::dbSta* sta)
    : arch_(arch), network_(network),sta_(sta)
{
}
// DetailedMis::(Architecture* arch, Network* network,sta::dbSta* sta)
//     : arch_(arch), network_(network),sta_(sta)
// {
// }
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
DetailedMis::~DetailedMis()
{
  clearGrid();
}

//////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////
void DetailedMis::run(DetailedMgr* mgrPtr, const std::string& command)
{
  // A temporary interface to allow for a string which we will decode to create
  // the arguments.
  boost::char_separator<char> separators(" \r\t\n;");
  boost::tokenizer<boost::char_separator<char>> tokens(command, separators);
  std::vector<std::string> args;
  for (const auto& token : tokens) {
    args.push_back(token);
  }
  run(mgrPtr, args);
  //mgrPtr_->getLogger()->info(DPL, bajiujiu, "solveMatch cost {:.6f} s", solveMatchTimeUsage);
}

//////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////
void DetailedMis::run(DetailedMgr* mgrPtr, std::vector<std::string>& args)
{
  // Given the arguments, figure out which routine to run to do the reordering.

  mgrPtr_ = mgrPtr;

  // Defaults.
  obj_ = DetailedMis::Hpwl;

  int passes = 1;
  double tol = 0.01;
  for (size_t i = 1; i < args.size(); i++) {
    if (args[i] == "-p" && i + 1 < args.size()) {
      passes = std::atoi(args[++i].c_str());
    } else if (args[i] == "-t" && i + 1 < args.size()) {
      tol = std::atof(args[++i].c_str());
    } else if (args[i] == "-d") {
      obj_ = DetailedMis::Disp;
    }
  }
  tol = std::max(tol, 0.01);
  passes = std::max(passes, 1);

  const char* obj_str
      = (obj_ == DetailedMis::Hpwl) ? "wirelength" : "displacement";
  mgrPtr_->getLogger()->info(
      DPL, 300, "Set matching objective is {:s}.", obj_str);

  // If using displacement objective, then it isn't required to use colors.
  if (obj_ == DetailedMis::Disp) {
    useSameColor_ = false;
  }

  mgrPtr_->resortSegments();
  uint64_t hpwl_x, hpwl_y;
  int64_t curr_hpwl = Utility::hpwl(network_, hpwl_x, hpwl_y);
  const int64_t init_hpwl = curr_hpwl;

  double tot_disp, max_disp, avg_disp;
  double curr_disp = Utility::disp_l1(network_, tot_disp, max_disp, avg_disp);
  const double init_disp = curr_disp;

  // Do some things that only need to be done once regardless
  // of the number of passes.
  collectMovableCells();  // Movable cells.
  if (candidates_.empty()) {
    mgrPtr_->getLogger()->info(DPL, 202, "No movable cells found");
    return;
  }
  colorCells();  // Color the cells.
  buildGrid();   // Grid for searching for neigbours.

  for (int p = 1; p <= passes; p++) {
    const double curr_obj = (obj_ == DetailedMis::Hpwl) ? curr_hpwl : curr_disp;

    mgrPtr_->getLogger()->info(
        DPL, 301, "Pass {:3d} of matching; objective is {:.6e}.", p, curr_obj);

    // Run the algo here...
    place();

    const int64_t last_hpwl = curr_hpwl;
    curr_hpwl = Utility::hpwl(network_, hpwl_x, hpwl_y);
    if (obj_ == DetailedMis::Hpwl
        && std::abs(curr_hpwl - last_hpwl) / (double) last_hpwl <= tol) {
      break;
    }
    const double last_disp = curr_disp;
    curr_disp = Utility::disp_l1(network_, tot_disp, max_disp, avg_disp);
    if (obj_ == DetailedMis::Disp
        && std::fabs(curr_disp - last_disp) / last_disp <= tol) {
      break;
    }
  }
  mgrPtr_->resortSegments();

  double curr_imp;
  double curr_obj;
  if (obj_ == DetailedMis::Hpwl) {
    const double hpwl_imp
        = (((init_hpwl - curr_hpwl) / (double) init_hpwl) * 100.);
    curr_imp = hpwl_imp;
    curr_obj = curr_hpwl;
  } else {
    const double disp_imp = (((init_disp - curr_disp) / init_disp) * 100.);
    curr_imp = disp_imp;
    curr_obj = curr_disp;
  }
  mgrPtr_->getLogger()->info(
      DPL,
      302,
      "End of matching; objective is {:.6e}, improvement is {:.2f} percent.",
      curr_obj,
      curr_imp);
}

//////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////
void DetailedMis::place()
{
  //hyx changed
  node_slack_.resize(network_->getNumNodes(), 0.0f);

  sta::dbNetwork* dbnet = sta_->getDbNetwork();   
  
  const auto kMax = sta::MinMax::max();           
  
  for (auto& n_uptr : network_->getNodes()) {    
    Node* n = n_uptr.get();
    float worst = 0.0f;
    odb::dbInst* inst = n->getDbInst();
    if (!inst) continue;
    for (odb::dbITerm* iterm : inst->getITerms()) {
      if (auto* sta_pin = dbnet->dbToSta(iterm)) {   // 转换函数
        float s = sta_->pinSlack(sta_pin, kMax);  // ✔直接拿 pinSlack
        worst = std::min(worst, s);
      }
    }
    node_slack_[n->getId()] = (worst < 0.0f) ? -worst : 0.0f;
  }
  // Populate the grid.  Used for searching.
  populateGrid();

  timesUsed_.resize(network_->getNumNodes());
  std::fill(timesUsed_.begin(), timesUsed_.end(), 0);

  // 1) 提前分配 neighbours 数组
  seed_neighbours_.clear();
  //seed_neighbours_.resize(candidates_.size()); 
  seed_neighbours_.resize(network_->getNumNodes()); // hyx changed

  //std::vector<char> has_neigh(candidates_.size(), 0);
  std::vector<char> has_neigh(network_->getNumNodes(), 0);

  // 2) 单线程收集邻居
  for (const Node* seed : candidates_) {
    size_t idx = seed->getId();          // Node id 可直接用来索引
    if (gatherNeighbours(const_cast<Node*>(seed), seed_neighbours_[idx])) {
      has_neigh[idx] = 1;
    }
  }

//   // 3) 并行求解
// /* ------------------- seed_neighbours_ 已经准备好 ------------------- */
//   #pragma omp parallel for schedule(dynamic)
//   for (size_t k = 0; k < candidates_.size(); ++k) {
//     Node* seed = candidates_[k];

//     /* 跳过已用过或没有邻居的种子 */
//     if (timesUsed_[seed->getId()] >= maxTimesUsed_)  continue;
//     if (!has_neigh[seed->getId()])                   continue;

//     /* 尝试锁住 3×3 桶邻域；失败则让出 CPU 去处理别的种子 */
//     Bucket* bkt = cellToBinMap_[seed];
//     if (!tryLockNeighbourhood(bkt))                  continue;

//     /* ------------ 在拿到锁的安全区域内运行 solveMatch ---------------- */
//     solveMatch(seed_neighbours_[seed->getId()]);
//     /* ---------------------------------------------------------------- */

//     unlockNeighbourhood();           // 释放桶锁

//     /* 更新使用计数（原子自增，避免竞争） */
//     #pragma omp atomic
//     ++timesUsed_[seed->getId()];
//   }



/*------------------ 0. 预备 worklist = 所有下标 -------------------*/
  std::vector<int> worklist(candidates_.size());
  std::iota(worklist.begin(), worklist.end(), 0);

  /* 可选：随机乱序一遍，提高负载均衡 */
  /* 每一轮（或初始化时）乱序下标列表 */
  std::shuffle(worklist.begin(),worklist.end(),std::mt19937{std::random_device{}()});

  auto t_solveMatchStart2 = std::chrono::steady_clock::now(); //hyx changed
  const int MAX_ROUNDS = 5000;
  for (int round = 0; round < MAX_ROUNDS && !worklist.empty(); ++round) {
    /* ---- 1. 每个线程本地收集 retry ---- */
    thread_local std::vector<int> local_retry;
    local_retry.clear();

    #pragma omp parallel for schedule(dynamic)
    for (size_t idx = 0; idx < worklist.size(); ++idx) {
      int k = worklist[idx];
      Node* seed = candidates_[k];

      /* Skip 条件 */
      if (timesUsed_[seed->getId()] >= maxTimesUsed_)  continue;
      if (!has_neigh[seed->getId()])                   continue;

      Bucket* bkt = cellToBinMap_[seed];
      if (!tryLockNeighbourhood(bkt)) {
        local_retry.push_back(k);          // 抢不到锁，稍后重试
        continue;
      }

      /* ---------- 安全区 ---------- */
      solveMatch(seed_neighbours_[seed->getId()]);
      unlockNeighbourhood();
      //#pragma omp atomic
      ++timesUsed_[seed->getId()];
    }

    /* ---- 2. 合并 retry 列表 ---- */
    std::vector<int> new_worklist;
    {
      /* critical 只包围 push_back 合并，开销极小 */
      #pragma omp parallel
      {
        /* 复制 TLS retry 向量 */
        auto tmp = local_retry;    local_retry.clear();
        #pragma omp critical
        new_worklist.insert(new_worklist.end(),
                            tmp.begin(), tmp.end());
      }
    }

    // /* ---- 3. 下一轮准备 ---- */
    worklist.swap(new_worklist);
    if (!worklist.empty())
    {
        std::shuffle(worklist.begin(),worklist.end(),std::mt19937{std::random_device{}()});
    }
         // 再随机一次
  }
  std::cout<<"solveMatch cost "
           << std::chrono::duration<double>(std::chrono::steady_clock::now() - t_solveMatchStart2).count()
           << " s" << std::endl; //hyx changed



  // Select candidates and solve matching problem.  Note that we need to do
  // something to make this more efficient, otherwise we will solve way too
  // many problems for larger circuits.  I think one effective idea is to
  // keep track of how many problems a candidate cell has been involved in;
  // if it has been involved is >= a certain number of problems, it has "had
  // some chance" to be moved, so skip it.
  //mgrPtr_->shuffle(candidates_);
//   std::vector<bool> gatherNeighboursSuccess(candidates_.size(), false);

//   for(Node* ndi : candidates_) {  // Pick a candidate as a seed.

//     // Get other cells within the vicinity.
//     gatherNeighboursSuccess[ndi->getId()] = gatherNeighbours(ndi);

//   }
// #pragma omp parallel for schedule(dynamic)
//   for (Node* ndi : candidates_) {  // Pick a candidate as a seed.
//     // Skip seed if it has been used already.
//     if (timesUsed_[ndi->getId()] >= maxTimesUsed_) {
//       continue;
//     }

//     // Get other cells within the vicinity.
//     // if (!gatherNeighbours(ndi)) {
//     //   continue;
//     // }
//     //gatherNeighboursSuccess[ndi->getId()] = gatherNeighbours(ndi);
//     if( !gatherNeighboursSuccess[ndi->getId()]) {
//       continue;
//     }
    

//     // Solve the flow.
//     solveMatch();
//     // auto t_flashStaStart = std::chrono::steady_clock::now(); //hyx changed
//     // //hyx changed
//     // for (Node* n : neighbours_) {
//     //   float worst = 0.0f;
      
//     //   /* ① 拿到该节点对应的 dbInst */
//     //   odb::dbInst* inst = n->getDbInst();
//     //   if (!inst)
//     //     continue;  // 终端或 filler，没有 dbInst
    
//     //   /* ② 遍历实例所有 ITerm，计算 slack */
//     //   for (odb::dbITerm* iterm : inst->getITerms()) {
//     //     if (auto* sta_pin = dbnet->dbToSta(iterm)) {
//     //       float s = sta_->pinSlack(sta_pin, kMax);
//     //       worst   = std::min(worst, s);
//     //     }
//     //   }
    
//     //   /* ③ 记录最坏负 slack 的绝对值 */
//     //   node_slack_[n->getId()] = (worst < 0.0f) ? -worst : 0.0f;
//     //   // std::cout  << n->getId() <<" " ;
//     // }
//     // // std::cout << std::endl;
//     // this->flashStaTimeUsage += std::chrono::duration<double>(std::chrono::steady_clock::now() - t_flashStaStart).count(); //hyx changed
//     // this->flashStaCount += 1; //hyx changed
    

//     // Update grid?  Or, do we need to even bother?
//     ;
//   }
}

//////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////
void DetailedMis::collectMovableCells()
{
  candidates_.clear();
  candidates_.insert(candidates_.end(),
                     mgrPtr_->getSingleHeightCells().begin(),
                     mgrPtr_->getSingleHeightCells().end());
  for (size_t i = 2; i < mgrPtr_->getNumMultiHeights(); i++) {
    candidates_.insert(candidates_.end(),
                       mgrPtr_->getMultiHeightCells(i).begin(),
                       mgrPtr_->getMultiHeightCells(i).end());
  }
}

//////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////
void DetailedMis::colorCells()
{
  colors_.resize(network_->getNumNodes());
  std::fill(colors_.begin(), colors_.end(), -1);

  movable_.resize(network_->getNumNodes());
  std::fill(movable_.begin(), movable_.end(), false);
  for (const Node* ndi : candidates_) {
    movable_[ndi->getId()] = true;
  }

  ColorGraph gr(network_->getNumNodes());
  for (int e = 0; e < network_->getNumEdges(); e++) {
    const Edge* edi = network_->getEdge(e);

    const int numPins = edi->getNumPins();
    if (numPins <= 1 || numPins > skipEdgesLargerThanThis_) {
      continue;
    }

    for (int pi = 0; pi < edi->getNumPins(); pi++) {
      const Pin* pini = edi->getPins()[pi];
      const Node* ndi = pini->getNode();
      if (!movable_[ndi->getId()]) {
        continue;
      }

      for (int pj = pi + 1; pj < edi->getNumPins(); pj++) {
        const Pin* pinj = edi->getPins()[pj];
        const Node* ndj = pinj->getNode();
        if (!movable_[ndj->getId()]) {
          continue;
        }
        if (ndj == ndi) {
          continue;
        }

        gr.addEdge(ndi->getId(), ndj->getId());
      }
    }
  }

  // The actual coloring.
  gr.greedyColoring();

  for (int i = 0; i < network_->getNumNodes(); i++) {
    const Node* ndi = network_->getNode(i);

    const int color = gr.getColor(i);
    if (color < 0 || color >= gr.getNumColors()) {
      mgrPtr_->internalError("Unable to color cells during matching");
    }
    if (movable_[ndi->getId()]) {
      colors_[ndi->getId()] = color;
    }
  }
}

//////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////
void DetailedMis::buildGrid()
{
  // Builds a coarse grid over the placement region for locating cells.
  traversal_ = 0;

  const double xmin = arch_->getMinX().v;
  const double xmax = arch_->getMaxX().v;
  const double ymin = arch_->getMinY().v;
  const double ymax = arch_->getMaxY().v;

  // Design each grid bin to hold a few hundred cells.  Do this based on the
  // average width and height of the cells.
  double avgH = 0.;
  double avgW = 0.;
  for (const Node* ndi : candidates_) {
    avgH += ndi->getHeight().v;
    avgW += ndi->getWidth().v;
  }
  avgH /= (double) candidates_.size();
  avgW /= (double) candidates_.size();

  stepX_ = avgW * std::sqrt(maxProblemSize_);
  stepY_ = avgH * std::sqrt(maxProblemSize_);

  dimW_ = (int) std::ceil((xmax - xmin) / stepX_);
  dimH_ = (int) std::ceil((ymax - ymin) / stepY_);

  clearGrid();
  grid_.resize(dimW_);
  for (int i = 0; i < dimW_; i++) {
    grid_[i].resize(dimH_);
    for (int j = 0; j < dimH_; j++) {
      auto bucket = new Bucket;
      bucket->xmin_ = xmin + (i) *stepX_;
      bucket->xmax_ = xmin + (i + 1) * stepX_;
      bucket->ymin_ = ymin + (j) *stepY_;
      bucket->ymax_ = ymin + (j + 1) * stepY_;
      bucket->i_ = i;
      bucket->j_ = j;
      bucket->travId_ = traversal_;
      grid_[i][j] = bucket;
    }
  }
  bucket_locks_.resize(dimW_);
  for (int i = 0; i < dimW_; ++i) 
  {
    bucket_locks_[i].resize(dimH_);
    for (int j = 0; j < dimH_; ++j) 
    {
      omp_init_lock(&bucket_locks_[i][j]);
    }
  }

}

//////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////
void DetailedMis::populateGrid()
{
  // Inserts movable cells into the grid.

  for (size_t i = 0; i < grid_.size(); i++) {
    for (size_t j = 0; j < grid_[i].size(); j++) {
      grid_[i][j]->clear();
    }
  }

  const double xmin = arch_->getMinX().v;
  const double ymin = arch_->getMinY().v;

  // Insert cells into the constructed grid.
  cellToBinMap_.clear();
  for (Node* ndi : candidates_) {
    const double y = ndi->getBottom().v + 0.5 * ndi->getHeight().v;
    const double x = ndi->getLeft().v + 0.5 * ndi->getWidth().v;

    const int j = std::max(std::min((int) ((y - ymin) / stepY_), dimH_ - 1), 0);
    const int i = std::max(std::min((int) ((x - xmin) / stepX_), dimW_ - 1), 0);

    grid_[i][j]->nodes_.push_back(ndi);
    cellToBinMap_[ndi] = grid_[i][j];
  }
}

//////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////
void DetailedMis::clearGrid()
// Clear out any old grid.  The dimensions of the grid are also stored in the
// class...
{

  for (auto& row : bucket_locks_) 
  {
    for (auto& lk : row) 
    {
      omp_destroy_lock(&lk);
    }
    row.clear();
  }
  bucket_locks_.clear();;

  for (auto& row : grid_) {
    for (auto bucket : row) {
      delete bucket;
    }
    row.clear();
  }
  grid_.clear();
}

/* ------------------------------------------------------------------ */
/* 锁住 seed 所在桶及其 8 个邻桶；失败则不做任何改动返回 false        */
/* ------------------------------------------------------------------ */
bool DetailedMis::tryLockNeighbourhood(Bucket* bkt)
{
  neighbour_hold_.clear();          // 确保为空

  auto grab = [&](int ii, int jj) -> bool {
    if (ii < 0 || ii >= dimW_ || jj < 0 || jj >= dimH_)
      return true;                  // 越界视作已锁
    omp_lock_t* lk = &bucket_locks_[ii][jj];
    if (!omp_test_lock(lk)) {       // 抢锁失败：回滚
      for (auto* p : neighbour_hold_)
        omp_unset_lock(p);
      neighbour_hold_.clear();
      return false;
    }
    neighbour_hold_.push_back(lk);
    return true;
  };

  int i = bkt->i_;
  int j = bkt->j_;

  /* 自己 + 上下左右 + 四个角（共 9 个） */
  return grab(i, j)   && grab(i-1, j) && grab(i+1, j) &&
         grab(i, j-1) && grab(i, j+1) &&
         grab(i-1,j-1)&& grab(i-1,j+1)&&
         grab(i+1,j-1)&& grab(i+1,j+1) &&
         grab(i+2,j) && grab(i-2,j) &&
         grab(i,j+2) && grab(i,j-2) &&
         grab(i+2,j+2) && grab(i-2,j-2) &&
         grab(i+2,j-2) && grab(i-2,j+2) &&
         grab(i+2,j+1) && grab(i-2,j+1) &&
         grab(i+2,j-1) && grab(i-2,j-1) &&
         grab(i+1,j+2) && grab(i-1,j+2) &&
         grab(i+1,j-2) && grab(i-1,j-2);
}

/* ------------------------------------------------------------------ */
/* 释放当前线程占用的所有桶锁                                          */
/* ------------------------------------------------------------------ */
void DetailedMis::unlockNeighbourhood()
{
  for (auto* lk : neighbour_hold_)
    omp_unset_lock(lk);
  neighbour_hold_.clear();
}



//////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////
bool DetailedMis::gatherNeighbours(Node* ndi, std::vector<Node*>& neigh_out) //hyx changed
{
  const double singleRowHeight = arch_->getRow(0)->getHeight().v;

  neigh_out.clear();
  neigh_out.push_back(ndi);

  // Scan the grid structure gathering up cells which are compatible with the
  // current cell.

  auto it = cellToBinMap_.find(ndi);
  if (it == cellToBinMap_.end()) {
    return false;
  }

  const int spanned_i = std::lround(ndi->getHeight().v / singleRowHeight);

  std::queue<Bucket*> Q;
  Q.push(it->second);
  ++traversal_;
  while (!Q.empty()) {
    Bucket* currPtr = Q.front();
    Q.pop();

    if (currPtr->travId_ == traversal_) {
      continue;
    }
    currPtr->travId_ = traversal_;

    // Scan all the cells in this bucket.  If they are compatible with the
    // original cell, then add them to the neighbour list.
    for (Node* ndj : currPtr->nodes_) {
      // Check to make sure the cell is not the original, that they have
      // the same region, that they have the same size (if applicable),
      // and that they have the same color (if applicable).

      // diff nodes
      if (ndj == ndi) {
        continue;
      }

      // Must be the same color to avoid sharing nets.
      if (useSameColor_ && colors_[ndi->getId()] != colors_[ndj->getId()]) {
        continue;
      }

      // Must be the same size.
      if (useSameSize_
          && (ndi->getWidth() != ndj->getWidth()
              || ndi->getHeight() != ndj->getHeight())) {
        continue;
      }

      // Must be in the same region.
      if (ndj->getGroupId() != ndi->getGroupId()) {
        continue;
      }

      // Must span the same number of rows and also be voltage compatible.
      if (ndi->getBottomPower() != ndj->getBottomPower()
          || ndi->getTopPower() != ndj->getTopPower()
          || spanned_i != std::lround(ndj->getHeight().v / singleRowHeight)) {
        continue;
      }

      // If compatible, include this current cell.
      neigh_out.push_back(ndj);
    }

    if (neigh_out.size() >= maxProblemSize_) {
      break;
    }

    // Add more bins to the queue if we have not yet collected enough cells.
    if (currPtr->i_ > 0) {
      Q.push(grid_[currPtr->i_ - 1][currPtr->j_]);
    }
    if (currPtr->i_ + 1 < dimW_) {
      Q.push(grid_[currPtr->i_ + 1][currPtr->j_]);
    }
    if (currPtr->j_ > 0) {
      Q.push(grid_[currPtr->i_][currPtr->j_ - 1]);
    }
    if (currPtr->j_ + 1 < dimH_) {
      Q.push(grid_[currPtr->i_][currPtr->j_ + 1]);
    }
  }
  return true;
}

//////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////
void DetailedMis::solveMatch(const std::vector<Node*>& neighbours_)
{
  auto t_solveStart = std::chrono::steady_clock::now(); // hyx changed
  if (neighbours_.size() <= 1) {
    return;
  }
  const std::vector<Node*>& nodes = neighbours_;

  const int nNodes = (int) nodes.size();
  const int nSpots = (int) nodes.size();

  // Original position of cells.
  std::vector<std::pair<DbuX, DbuY>> pos(nNodes);
  // Original segment assignment of cells.
  std::vector<std::vector<DetailedSeg*>> seg(nNodes);
  for (size_t i = 0; i < nodes.size(); i++) {
    Node* ndi = nodes[i];

    pos[i] = std::make_pair(ndi->getLeft(), ndi->getBottom());
    seg[i] = mgrPtr_->getReverseCellToSegs(ndi->getId());  // copy!
  }

  lemon::ListDigraph g;
  std::vector<lemon::ListDigraph::Node> nodeForCell;
  std::vector<lemon::ListDigraph::Node> nodeForSpot;
  nodeForCell.resize(nNodes);
  nodeForSpot.resize(nNodes);
  for (size_t i = 0; i < nodes.size(); i++) {
    nodeForCell[i] = g.addNode();
    nodeForSpot[i] = g.addNode();
  }
  lemon::ListDigraph::Node supplyNode = g.addNode();
  lemon::ListDigraph::Node demandNode = g.addNode();

  // Hook up the graph.
  lemon::ListDigraph::ArcMap<int> l_i(g);  // Lower bound on flow.
  lemon::ListDigraph::ArcMap<int> u_i(g);  // Upper bound on flow.
  lemon::ListDigraph::ArcMap<int> c_i(g);  // Cost of flow.

  std::map<lemon::ListDigraph::Arc, std::pair<int, int>> reverseMap;

  double icost;
  for (int i = 0; i < nNodes; i++) {
    // Supply to node.
    lemon::ListDigraph::Arc arc_sv = g.addArc(supplyNode, nodeForCell[i]);
    l_i[arc_sv] = 0;
    u_i[arc_sv] = 1;
    c_i[arc_sv] = 0;

    // Spot to demand.
    lemon::ListDigraph::Arc arc_vt = g.addArc(nodeForSpot[i], demandNode);
    l_i[arc_vt] = 0;
    u_i[arc_vt] = 1;
    c_i[arc_vt] = 0;

    // Nodes to spots.
    const Node* ndi = nodes[i];
    for (int j = 0; j < nSpots; j++) {
      // Determine the cost of assigning cell "ndi" to the
      // current position.  Note that we might want to
      // skip this location if it violates the maximum
      // displacement limit. We _never_ prevent a cell
      // from being assigned to its original position as
      // this guarantees a solution!
      if (i != j) {
        const DbuX dx = abs(pos[j].first - ndi->getOrigLeft());
        if (dx > mgrPtr_->getMaxDisplacementX()) {
          continue;
        }
        const DbuY dy = abs(pos[j].second - ndi->getOrigBottom());
        if (dy > mgrPtr_->getMaxDisplacementY()) {
          continue;
        }
      }

      // Okay to assign the cell to this location.
      // hyx changed icost
      if (obj_ == DetailedMis::Hpwl) {
        const float slack_cost = slack_wt_ * node_slack_[ndi->getId()];
        icost = getHpwl(ndi,
                        pos[j].first + ndi->getWidth() / 2,
                        pos[j].second + ndi->getHeight() / 2)*0.5 +getDisp(ndi, pos[j].first, pos[j].second)*0.5 +slack_cost*5;
      } else {
        icost = getDisp(ndi, pos[j].first, pos[j].second);
      }

      // Node to spot.
      lemon::ListDigraph::Arc arc_vu = g.addArc(nodeForCell[i], nodeForSpot[j]);
      l_i[arc_vu] = 0;
      u_i[arc_vu] = 1;
      c_i[arc_vu] = icost > std::numeric_limits<int>::max()
                        ? std::numeric_limits<int>::max()
                        : static_cast<int>(icost);

      reverseMap[arc_vu] = std::make_pair(i, j);
    }
  }
  // Try max flow.

  auto t_maxFlowStart = std::chrono::steady_clock::now(); // hyx changed

  lemon::Preflow<lemon::ListDigraph> preflow(g, u_i, supplyNode, demandNode);
  preflow.run();
  const int maxFlow = preflow.flowValue();
  if (maxFlow != nNodes) {
    return;
  }
  // Find mincost flow.
  lemon::NetworkSimplex<lemon::ListDigraph> mincost(g);
  mincost.lowerMap(l_i);
  mincost.upperMap(u_i);
  mincost.costMap(c_i);
  mincost.stSupply(supplyNode, demandNode, maxFlow);
  // lemon::CycleCanceling<lemon::ListDigraph>::ProblemType ret = mincost.run();
  lemon::NetworkSimplex<lemon::ListDigraph>::ProblemType ret = mincost.run();
  if (ret != lemon::NetworkSimplex<lemon::ListDigraph>::OPTIMAL) {
    return;
  }

  // Get the solution and assign nodes to new spots.  We also need to update the
  // assignment of cells to segments!  I _believe_ it should be fine to go cell
  // by cell and remove, reposition and update segment assignments one-by-one.
  //
  // This is somewhat tricky.  We need to use the target spot to figure out the
  // segments into which the cell needs to be replaced.

  lemon::ListDigraph::ArcMap<int> flow(g);
  mincost.flowMap(flow);

  this->maxFlowTimeUsage += std::chrono::duration<double>(std::chrono::steady_clock::now()-t_maxFlowStart).count(); //hyx changed
  this->maxFlowCount += 1; //hyx changed
  
  //#pragma omp critical(seg_move)
  //{
    Journal journal(mgrPtr_->getGrid(), mgrPtr_);
    for (lemon::ListDigraph::ArcMap<int>::ItemIt it(flow); it != lemon::INVALID;
        ++it) {
      if (g.target(it) != demandNode && g.source(it) != supplyNode
          && mincost.flow(it) != 0) {
        auto it1 = reverseMap.find(it);
        if (reverseMap.end() == it1) {
          mgrPtr_->internalError("Unable to interpret flow during matching");
        }

        const int i = it1->second.first;
        const int j = it1->second.second;

        // If cell "i" is assigned to location "i", it means that it has not
        // moved. We don't need to remove and reinsert it...

        Node* ndi = nodes[i];
        const Node* ndj = nodes[j];

        const int spanned_i = arch_->getCellHeightInRows(ndi);
        const int spanned_j = arch_->getCellHeightInRows(ndj);

        if (ndi != ndj) {
          if (spanned_i != spanned_j || ndi->getWidth() != ndj->getWidth()
              || ndi->getHeight() != ndj->getHeight()) {
            mgrPtr_->internalError("Unable to interpret flow during matching");
          }

          // Remove cell "i" from its old segments.
          std::vector<DetailedSeg*>& old_segs = seg[i];
          if (spanned_i != old_segs.size()) {
            // This means an error someplace else...
            mgrPtr_->internalError("Unable to interpret flow during matching");
          }
          std::vector<int> old_seg_ids;
          old_seg_ids.reserve(old_segs.size());
          for (const DetailedSeg* segPtr : old_segs) {
            const int segId = segPtr->getSegId();
            old_seg_ids.push_back(segId);
            mgrPtr_->removeCellFromSegment(ndi, segId);
          }

          // Update the postion of cell "i".
          mgrPtr_->eraseFromGrid(ndi);
          ndi->setLeft(DbuX{pos[j].first});
          ndi->setBottom(pos[j].second);
          mgrPtr_->paintInGrid(ndi);

          // Determine new segments and add cell "i" to its new segments.
          const std::vector<DetailedSeg*>& new_segs = seg[j];
          if (spanned_i != new_segs.size()) {
            // Not setup for non-same size stuff right now.
            mgrPtr_->internalError("Unable to interpret flow during matching");
          }
          std::vector<int> new_seg_ids;
          new_seg_ids.reserve(new_segs.size());

          for (const DetailedSeg* segPtr : new_segs) {
            const int segId = segPtr->getSegId();
            new_seg_ids.push_back(segId);
            mgrPtr_->addCellToSegment(ndi, segId);
          }
          {
            JournalAction action;
            action.setType(JournalAction::MOVE_CELL);
            action.setNode(ndi);
            action.setOrigLocation(pos[i].first, pos[i].second);
            action.setNewLocation(pos[j].first, pos[j].second);
            action.setOrigSegs(old_seg_ids);
            action.setNewSegs(new_seg_ids);
            journal.addAction(action);
          }
        }
      }
    }
    bool viol = false;
    for (const auto node : nodes) {
      if (mgrPtr_->hasEdgeSpacingViolation(node)) {
        viol = true;
        break;
      }
    }
    if (viol) {
      while (!journal.isEmpty()) {
        const auto& action = journal.getLastAction();
        journal.undo(action);
        journal.removeLastAction();
      }
    }
  
  //}
  this->solveMatchTimeUsage += std::chrono::duration<double>(std::chrono::steady_clock::now()-t_solveStart).count();//hyx changed
  //mgrPtr_->getLogger()->info(DPL, bajiujiu, "solveMatch cost {:.6f} s", solveMatchTimeUsage); //hyx changed
}

//////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////
uint64_t DetailedMis::getDisp(const Node* ndi, DbuX xi, DbuY yi)
{
  // Compute displacement of cell ndi if placed at (xi,y1) from its orig pos.
  const DbuX dx = abs(xi - ndi->getOrigLeft());
  const DbuY dy = abs(yi - ndi->getOrigBottom());
  return dx.v + dy.v;
}

//////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////
uint64_t DetailedMis::getHpwl(const Node* ndi, DbuX xi, DbuY yi)
{
  // Compute the HPWL of nets connected to ndi assuming ndi is at the
  // specified (xi,yi).

  uint64_t hpwl = 0.;
  for (int pi = 0; pi < ndi->getNumPins(); pi++) {
    const Pin* pini = ndi->getPins()[pi];

    const Edge* edi = pini->getEdge();

    const int npins = edi->getNumPins();
    if (npins <= 1 || npins > skipEdgesLargerThanThis_) {
      continue;
    }
    odb::Rect box;
    box.mergeInit();
    for (int pj = 0; pj < edi->getNumPins(); pj++) {
      const Pin* pinj = edi->getPins()[pj];

      const Node* ndj = pinj->getNode();

      const DbuX x = (ndj == ndi) ? (xi + pinj->getOffsetX())
                                  : (ndj->getCenterX() + pinj->getOffsetX());
      const DbuY y = (ndj == ndi) ? (yi + pinj->getOffsetY())
                                  : (ndj->getCenterY() + pinj->getOffsetY());

      box.merge(odb::Point(x.v, y.v));
    }
    if (box.xMax() >= box.xMin() && box.yMax() >= box.yMin()) {
      hpwl += (box.dx() + box.dy());
    }
  }

  return hpwl;
}

}  // namespace dpl
