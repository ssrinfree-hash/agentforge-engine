// AgentForge — core math (header-only). C++17, ноль зависимостей.
// Vec2/3/4, Mat4, Quat, AABB, преобразования, перспективная камера.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace af {

constexpr float PI = 3.14159265358979f;

struct Vec2 { float x=0, y=0;
  Vec2()=default; Vec2(float a,float b):x(a),y(b){}
  Vec2 operator+(const Vec2&o)const{return {x+o.x,y+o.y};}
  Vec2 operator-(const Vec2&o)const{return {x-o.x,y-o.y};}
  Vec2 operator*(float s)const{return {x*s,y*s};}
  float dot(const Vec2&o)const{return x*o.x+y*o.y;}
  float len()const{return std::sqrt(x*x+y*y);} };

struct Vec3 { float x=0, y=0, z=0;
  Vec3()=default; Vec3(float a,float b,float c):x(a),y(b),z(c){}
  Vec3 operator+(const Vec3&o)const{return {x+o.x,y+o.y,z+o.z};}
  Vec3 operator-(const Vec3&o)const{return {x-o.x,y-o.y,z-o.z};}
  Vec3 operator-()const{return {-x,-y,-z};}
  Vec3 operator*(float s)const{return {x*s,y*s,z*s};}
  Vec3 operator*(const Vec3&o)const{return {x*o.x,y*o.y,z*o.z};}   // component-wise
  Vec3& operator+=(const Vec3&o){x+=o.x;y+=o.y;z+=o.z;return *this;}
  Vec3& operator*=(float s){x*=s;y*=s;z*=s;return *this;}
  float dot(const Vec3&o)const{return x*o.x+y*o.y+z*o.z;}
  Vec3 cross(const Vec3&o)const{
    return {y*o.z-z*o.y, z*o.x-x*o.z, x*o.y-y*o.x};}
  float len()const{return std::sqrt(x*x+y*y+z*z);}
  Vec3 normalized()const{float l=len();return l>1e-9f?Vec3{x/l,y/l,z/l}:Vec3{};}
  float dist(const Vec3&o)const{return (*this-o).len();} };

struct Vec4 { float x=0,y=0,z=0,w=0;
  Vec4()=default; Vec4(float a,float b,float c,float d):x(a),y(b),z(c),w(d){} };

struct Quat { float x=0,y=0,z=0,w=1;   // (x,y,z) вектор, w — скаляр
  Quat()=default; Quat(float a,float b,float c,float d):x(a),y(b),z(c),w(d){}
  static Quat identity(){return {};}
  static Quat from_axis_angle(const Vec3&axis,float rad){
    Vec3 a=axis.normalized(); float s=std::sin(rad*0.5f);
    return {a.x*s,a.y*s,a.z*s,std::cos(rad*0.5f)}; }
  static Quat from_euler(float rx,float ry,float rz){ // XYZ, радианы
    Quat qx=from_axis_angle({1,0,0},rx), qy=from_axis_angle({0,1,0},ry),
         qz=from_axis_angle({0,0,1},rz);
    return (qy*qx)*qz; }
  Quat operator*(const Quat&o)const{
    return {w*o.x+x*o.w+y*o.z-z*o.y,
            w*o.y-x*o.z+y*o.w+z*o.x,
            w*o.z+x*o.y-y*o.x+z*o.w,
            w*o.w-x*o.x-y*o.y-z*o.z};}
  Quat normalized()const{float l=std::sqrt(x*x+y*y+z*z+w*w);
    return l>1e-9f?Quat{x/l,y/l,z/l,w/l}:identity();}
  Vec3 rotate(const Vec3&v)const{           // v' = q*v*q^-1
    Vec3 u{x,y,z}; float w2=w;
    return u*2.0f*u.dot(v) + v*(w2*w2-u.dot(u)) + u.cross(v)*2.0f*w2; }
  static Quat slerp(const Quat&a,const Quat&b,float t){
    float d=a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w;
    Quat bb=d<0?Quat{-b.x,-b.y,-b.z,-b.w}:b; d=std::fabs(d);
    if(d>0.9995f){ Quat r{(a.x+(bb.x-a.x)*t),(a.y+(bb.y-a.y)*t),
      (a.z+(bb.z-a.z)*t),(a.w+(bb.w-a.w)*t)}; return r.normalized(); }
    float th=std::acos(d), s=std::sin(th);
    float wa=std::sin((1-t)*th)/s, wb=std::sin(t*th)/s;
    return {a.x*wa+bb.x*wb, a.y*wa+bb.y*wb, a.z*wa+bb.z*wb, a.w*wa+bb.w*wb}; } };

struct Mat4 { float m[16];               // column-major (как в OpenGL)
  Mat4(){ std::memset(m,0,sizeof m); m[0]=m[5]=m[10]=m[15]=1; }
  static Mat4 zeros(){ Mat4 r; std::memset(r.m,0,sizeof r.m); return r; }
  float& at(int r,int c){return m[c*4+r];}
  float at(int r,int c)const{return m[c*4+r];}
  Mat4 operator*(const Mat4&o)const{
    Mat4 r=Mat4::zeros();
    for(int c=0;c<4;c++)for(int rr=0;rr<4;rr++){
      float s=0; for(int k=0;k<4;k++) s+=at(rr,k)*o.at(k,c); r.at(rr,c)=s; }
    return r; }
  static Mat4 translation(const Vec3&t){ Mat4 r; r.at(0,3)=t.x; r.at(1,3)=t.y; r.at(2,3)=t.z; return r; }
  static Mat4 scale(const Vec3&s){ Mat4 r; r.m[0]=s.x; r.m[5]=s.y; r.m[10]=s.z; return r; }
  static Mat4 from_quat(const Quat&q){
    Mat4 r;
    float xx=q.x*q.x,yy=q.y*q.y,zz=q.z*q.z,xy=q.x*q.y,xz=q.x*q.z,yz=q.y*q.z,
          wx=q.w*q.x,wy=q.w*q.y,wz=q.w*q.z;
    r.m[0]=1-2*(yy+zz); r.m[1]=2*(xy+wz);   r.m[2]=2*(xz-wy);
    r.m[4]=2*(xy-wz);   r.m[5]=1-2*(xx+zz); r.m[6]=2*(yz+wx);
    r.m[8]=2*(xz+wy);   r.m[9]=2*(yz-wx);   r.m[10]=1-2*(xx+yy);
    return r; }
  static Mat4 trs(const Vec3&t,const Quat&q,const Vec3&s){
    return translation(t)*from_quat(q)*scale(s); }
  static Mat4 perspective(float fov_y_rad,float aspect,float zn,float zf){
    float f=1.0f/std::tan(fov_y_rad*0.5f); Mat4 r=Mat4::zeros();
    r.at(0,0)=f/aspect; r.at(1,1)=f;
    r.at(2,2)=(zf+zn)/(zn-zf);      // GL: (far+near)/(near-far)
    r.at(2,3)=(2*zf*zn)/(zn-zf);    // GL: 2*far*near/(near-far)
    r.at(3,2)=-1;                   // clip.w = -view_z (точка впереди камеры)
    return r; }
  static Mat4 orthographic(float l,float r_,float b,float t,float zn,float zf){
    Mat4 o=Mat4::zeros();
    o.at(0,0)=2/(r_-l); o.at(1,1)=2/(t-b); o.at(2,2)=2/(zn-zf);
    o.at(0,3)=-(r_+l)/(r_-l); o.at(1,3)=-(t+b)/(t-b); o.at(2,3)=-(zf+zn)/(zf-zn);
    o.at(3,3)=1; return o; }
  static Mat4 look_at(const Vec3&eye,const Vec3&center,const Vec3&up){
    Vec3 f=(center-eye).normalized(); Vec3 s=f.cross(up).normalized(); Vec3 u=s.cross(f);
    Mat4 r; r.at(0,0)=s.x; r.at(0,1)=s.y; r.at(0,2)=s.z;
    r.at(1,0)=u.x; r.at(1,1)=u.y; r.at(1,2)=u.z;
    r.at(2,0)=-f.x; r.at(2,1)=-f.y; r.at(2,2)=-f.z;
    r.at(0,3)=-s.dot(eye); r.at(1,3)=-u.dot(eye); r.at(2,3)=f.dot(eye); return r; }
  Vec3 transform_point(const Vec3&p)const{
    return { at(0,0)*p.x+at(0,1)*p.y+at(0,2)*p.z+at(0,3),
             at(1,0)*p.x+at(1,1)*p.y+at(1,2)*p.z+at(1,3),
             at(2,0)*p.x+at(2,1)*p.y+at(2,2)*p.z+at(2,3) }; }
  Vec4 transform_point4(const Vec3&p)const{   // полный clip-space (w = -view_z)
    return { at(0,0)*p.x+at(0,1)*p.y+at(0,2)*p.z+at(0,3),
             at(1,0)*p.x+at(1,1)*p.y+at(1,2)*p.z+at(1,3),
             at(2,0)*p.x+at(2,1)*p.y+at(2,2)*p.z+at(2,3),
             at(3,0)*p.x+at(3,1)*p.y+at(3,2)*p.z+at(3,3) }; }
  Vec3 transform_dir(const Vec3&p)const{
    return { at(0,0)*p.x+at(0,1)*p.y+at(0,2)*p.z,
             at(1,0)*p.x+at(1,1)*p.y+at(1,2)*p.z,
             at(2,0)*p.x+at(2,1)*p.y+at(2,2)*p.z }; }
  Mat4 inverse() const {              // Гаусс-Жордан, устойчиво и без кофакторных опечаток
    float a[4][8];
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) { a[r][c] = m[c * 4 + r]; a[r][c + 4] = (r == c) ? 1.0f : 0.0f; }
    for (int col = 0; col < 4; ++col) {
      int pivot = col;
      for (int r = col + 1; r < 4; ++r)
        if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) pivot = r;
      if (std::fabs(a[pivot][col]) < 1e-12f) return Mat4{};   // вырожденная
      if (pivot != col)
        for (int c = 0; c < 8; ++c) std::swap(a[col][c], a[pivot][c]);
      float d = a[col][col];
      for (int c = 0; c < 8; ++c) a[col][c] /= d;
      for (int r = 0; r < 4; ++r) {
        if (r == col) continue;
        float f = a[r][col];
        if (f == 0) continue;
        for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
      }
    }
    Mat4 r = Mat4::zeros();
    for (int rr = 0; rr < 4; ++rr)
      for (int c = 0; c < 4; ++c) r.at(rr, c) = a[rr][c + 4];
    return r; } };

struct AABB { Vec3 mn, mx; bool empty=true;
  AABB()=default; AABB(Vec3 a,Vec3 b):mn(a),mx(b),empty(false){}
  void grow(const Vec3&p){ if(empty){mn=mx=p;empty=false;return;}
    mn.x=std::min(mn.x,p.x);mn.y=std::min(mn.y,p.y);mn.z=std::min(mn.z,p.z);
    mx.x=std::max(mx.x,p.x);mx.y=std::max(mx.y,p.y);mx.z=std::max(mx.z,p.z); }
  Vec3 center()const{return {(mn.x+mx.x)*0.5f,(mn.y+mx.y)*0.5f,(mn.z+mx.z)*0.5f};}
  Vec3 size()const{return {mx.x-mn.x,mx.y-mn.y,mx.z-mn.z};}
  bool intersects(const AABB&o)const{
    return !empty&&!o.empty&&
      mn.x<=o.mx.x&&mx.x>=o.mn.x&&mn.y<=o.mx.y&&mx.y>=o.mn.y&&mn.z<=o.mx.z&&mx.z>=o.mn.z;}
  bool contains(const Vec3&p)const{
    return !empty&&p.x>=mn.x&&p.x<=mx.x&&p.y>=mn.y&&p.y<=mx.y&&p.z>=mn.z&&p.z<=mx.z;} };

} // namespace af
