#ifndef MPIKMEAN_H
#define MPIKMEAN_H

#include<boost/mpi.hpp>
#include<boost/serialization/vector.hpp>
#include<boost/timer.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/any.hpp>
#include <boost/random.hpp>
#include<iostream>
#include "../datasets/dataset.hpp"
#include "../utilities/types.hpp"
using namespace std;

template<typename T>
struct vplus {
    vector<T> operator()(const vector<T> &x,const vector<T> &y) 
    {
        vector<T> result = x;
        for(size_t i=0; i<x.size(); ++i) 
        {
            result[i] += y[i];
        }
    return result;
    }
};

class Additional 
{
    public: 
      const::boost::any &get(const string& name) const;
      void insert(const string& name,const boost::any &val);
      map<string,boost::any> additional;
    protected:
      Additional(){}
};
///////
const boost::any& Additional::get(const string& name) const 
{
    map<string,boost::any>::const_iterator it;
    it = additional.find(name);
    if(it==additional.end())
    {
        FAIL(name << "not found");
    }
    return it->second;
}
void Additional::insert(const string& name,const boost::any& val)
{
    additional.insert(pair<string,boost::any>(name,val));
}


/////////////////////
class Arguments:public Additional
{
    public:
       boost::shared_ptr<Dataset> ds;

};
class Results:public Additional  
{
    public:
        void reset();
        vector<Size> CM;
};
void Results::reset() {
        CM.clear();
        additional.clear();
    }
    
//////////////////////
class MPIKmean
{
    public:
       Arguments& getArguments();//获取初始参数
       const Results& getResults() const;//获取结果_CM
       void reset() const;//清除结果
       void clusterize();//执行计算(初始化,更新,迭代,...)
    protected: 
        void setupArguments();//设置初始参数
        void fetchResults() const;//获取结果
        virtual void initialization() const;//随机初始中心簇
        virtual void iteration() const;//迭代更新
        virtual Real dist(Size i, Size j) const;//返回与中心簇的距离
        mutable vector<Real> _centers;//中心簇的属性值
        mutable vector<Real> _data;//数据值
        mutable Size _numObj;//分发给每一个线程的数据量
        mutable Size _numAttr;//数据属性量
        mutable vector<Size> _CM;//数据的所属簇index

        mutable vector<boost::shared_ptr<CenterCluster> > 
            _clusters;//中心簇
        mutable Real _error;//簇之间的总距离
        mutable Size _numiter;//迭代次数
        mutable Results _results;//结果
        boost::shared_ptr<Dataset> _ds;//dataset
        Arguments _arguments;
        Size _numclust;//聚类数目
        Size _maxiter;//最大迭代数目
        Size _seed;//种子
        boost::mpi::communicator _world;//mpi通信
};
///////////////////////
Arguments &MPIKmean::getArguments()
{
    return _arguments;
}
const Results& MPIKmean::getResults() const 
{
    return _results;
}
void MPIKmean::reset() const 
{
    _results.reset();
}

void MPIKmean::clusterize()
{
    setupArguments();
    initialization(); 
    iteration(); 
    reset();
    fetchResults();
}
void MPIKmean::setupArguments() { 
        _numclust = boost::any_cast<Size>(
            _arguments.get("numclust")); 
        _maxiter = boost::any_cast<Size>(
            _arguments.get("maxiter")); 
        ASSERT(_maxiter>0, "invalide maxiter");
        _seed = boost::any_cast<Size>(
            _arguments.get("seed")); 
        ASSERT(_seed>0, "invalide seed"); 
        if(_world.rank() ==0) {
            _ds = _arguments.ds; 
            ASSERT(_ds, "dataset is null");
            ASSERT(_ds->is_numeric(), "dataset is not numeric");
            ASSERT(_numclust>=2 && _numclust<=_ds->size(), 
                "invalid numclust");
        }
    }

void MPIKmean::fetchResults() const {
        Real error = 0.0;
        Real totalerror = 0.0;
        for(Size i=0;i<_numObj;++i) {
            error += dist(i,_CM[i]);
        } 
        reduce(_world, error, totalerror, plus<Real>(), 0);
        if(_world.rank() == 0) { 
            boost::shared_ptr<Schema> schema = _ds->schema();
            PClustering pc;
            for(Size i=0;i<_numclust;++i){
                for(Size j=0; j<_numAttr; ++j) { 
                    (*schema)[j]->set_c_val(
                        (*_clusters[i]->center())[j], 
                        _centers[i*_numAttr+j]);
                }
                pc.add(_clusters[i]);
            }

            for(Size i=0; i<_CM.size(); ++i) {
                _clusters[_CM[i]]->add((*_ds)[i]);
            }
            _results.CM = _CM;
            _results.insert("pc", boost::any(pc));
            _error = totalerror;
            _results.insert("error", boost::any(_error));
            _results.insert("numiter", boost::any(_numiter));
        }
    }
void MPIKmean::iteration() const {
        vector<Size> nChanged(1,1);//初始化nChanged,表示中心簇是否有变化.
        _numiter = 1;//初始迭代次数
        while(nChanged[0] > 0) { 
            nChanged[0] = 0;
            Size s;
            Real dMin,dDist;
            vector<Size> nChangedLocal(1,0);
            vector<Real> newCenters(_numclust*_numAttr,0.0);
            vector<Size> newSize(_numclust,0);
            for(Size i=0;i<_numObj;++i) {
                dMin = MAX_REAL;
                for(Size k=0;k<_numclust;++k) { 
                    dDist = dist(i, k);
                    if (dMin > dDist) {
                        dMin = dDist;
                        s = k;
                    }
                }
                for(Size j=0; j<_numAttr; ++j) {
                    newCenters[s*_numAttr+j] += 
                    		_data[i*_numAttr+j];
                }
                newSize[s] +=1;

                if (_CM[i] != s){
                    _CM[i] = s;
                    nChangedLocal[0]++;
                }
            }
            all_reduce(_world, nChangedLocal, nChanged, 
            		vplus<Size>());
            all_reduce(_world, newCenters, _centers, 
            		vplus<Real>()); 
            vector<Size> totalSize(_numclust,0);
            all_reduce(_world, newSize, totalSize, vplus<Size>()); 
            for(Size k=0; k<_numclust; ++k) {
                for(Size j=0; j<_numAttr; ++j) {
                    _centers[k*_numAttr+j] /= totalSize[k];
                }
            }
            ++_numiter;
            if (_numiter > _maxiter){
                break;
            }
        }
        if(_world.rank() > 0) {
            _world.send(0,0,_CM);
        } else {
            for(Size p=1; p<_world.size(); ++p) {
                vector<Size> msg;
                _world.recv(p,0,msg);
                for(Size j=0; j<msg.size(); ++j) {
                    _CM.push_back(msg[j]);
                }
            }
        }
    }
void MPIKmean::initialization() const {
    Size numRecords; 
    Size rank = _world.rank();
    //随机产生_clusters
    if (rank == 0) {//主进程负责
        numRecords = _ds->size(); 
        _numAttr = _ds->num_attr();
        _centers.resize(_numclust * _numAttr);
        vector<Integer> index(numRecords,0);
        for(Size i=0;i<index.size();++i){
            index[i] = i;
        }
        boost::shared_ptr<Schema> schema = _ds->schema();
        boost::minstd_rand generator(_seed);
        for(Size i=0;i<_numclust;++i){
            boost::uniform_int<> uni_dist(0,numRecords-i-1);
            boost::variate_generator<boost::minstd_rand&, 
                boost::uniform_int<> > 
                    uni(generator,uni_dist); 
            Integer r = uni();
            boost::shared_ptr<Record> cr = boost::shared_ptr
                <Record>(new Record(*(*_ds)[r]));
            boost::shared_ptr<CenterCluster> c = 
                boost::shared_ptr<CenterCluster>(
                    new CenterCluster(cr)); 
            c->set_id(i);
            _clusters.push_back(c);
            for(Size j=0; j<_numAttr; ++j) {
                _centers[i*_numAttr + j] = 
                    (*schema)[j]->get_c_val((*_ds)(r,j));
            }
            index.erase(index.begin()+r);
        } 
    } 
    boost::mpi::broadcast(_world, _centers, 0);
    boost::mpi::broadcast(_world, numRecords, 0);
    boost::mpi::broadcast(_world, _numAttr, 0);
    Size nDiv = numRecords / _world.size();
    Size nRem = numRecords % _world.size();
    if(rank == 0) { 
        boost::shared_ptr<Schema> schema = _ds->schema();
        _numObj = (nRem >0) ? nDiv+1: nDiv; 
        _data.resize(_numObj * _numAttr);
        _CM.resize(_numObj);
        for(Size i=0; i<_numObj; ++i) {
            for(Size j=0; j<_numAttr; ++j) {
                _data[i*_numAttr +j] = 
                    (*schema)[j]->get_c_val((*_ds)(i, j));
            }
        }
        Size nCount = _numObj; 
        for(Size p=1; p<_world.size(); ++p) {
            Size s = (p< nRem) ? nDiv +1 : nDiv;
            vector<Real> dv(s*_numAttr);
            for(Size i=0; i<s; ++i) {
                for(Size j=0; j<_numAttr; ++j) { 
                    dv[i*_numAttr+j] = 
                        (*schema)[j]->get_c_val(
                                (*_ds)(i+nCount,j));
                }
            }
            nCount += s;
            _world.send(p, 0, dv);
        }
    } else {
        _numObj = (rank < nRem) ? nDiv+1: nDiv; 
        _CM.resize(_numObj);
        _world.recv(0,0,_data);
    } 
} 
Real MPIKmean::dist(Size i, Size j) const {
        Real dDist = 0.0;
        for(Size h=0; h<_numAttr; ++h) {
            dDist += pow(_data[i*_numAttr + h] 
                    - _centers[j*_numAttr + h], 2.0);
        }
        return pow(dDist, 0.5);
    }
#endif