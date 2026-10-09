#include "frustum.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

static void require(bool value, char const *message) {
  if (!value) throw std::runtime_error(message);
}
int main() {
  try {
    auto clip = extractFrustum(glm::mat4{1});
    Aabb box{{-.1f,-.1f,.4f},{.1f,.1f,.6f},true};
    require(intersectsFrustum(clip,box),"Inside box rejected");
    for (unsigned axis=0;axis<3;++axis) for (int sign : {-1,1}) {
      auto outside=box;
      float offset=axis==2 ? (sign<0 ? -1.f : 1.f) : 2.f*sign;
      outside.min[axis]+=offset;outside.max[axis]+=offset;
      require(!intersectsFrustum(clip,outside),"Vulkan clip plane lost");
    }
    Aabb touching{{1,-.1f,.4f},{1.2f,.1f,.6f},true};
    require(intersectsFrustum(clip,touching),"Boundary contact rejected");
    touching.min.x=1.0005f;
    require(!intersectsFrustum(clip,touching),"Outside point retained without guard");
    require(intersectsFrustum(extractFrustum(glm::mat4{1},{.001f,.001f}),touching),
            "Subpixel guard failed to retain boundary geometry");
    auto perspective=glm::perspective(glm::radians(60.f),1.f,.1f,20.f);
    box.min.z=-1.1f;box.max.z=-.9f;
    require(intersectsFrustum(extractFrustum(perspective),box),"Perspective visible box rejected");
    box.min.z=.9f;box.max.z=1.1f;
    require(!intersectsFrustum(extractFrustum(perspective),box),"Behind-camera box retained");
    require(intersectsFrustum(clip,Aabb{}),"Missing bounds were not conservative");
    box.valid=true;box.min.x=std::numeric_limits<float>::quiet_NaN();
    require(intersectsFrustum(clip,box),"Nonfinite bounds were rejected");
    box.min.x=2;box.max.x=1;
    require(intersectsFrustum(clip,box),"Inverted bounds were rejected");
    auto invalid=glm::mat4{1};invalid[0][0]=std::numeric_limits<float>::infinity();
    require(intersectsFrustum(extractFrustum(invalid),touching),"Invalid frustum was not conservative");
    auto shifted=glm::translate(glm::mat4{1},glm::vec3{-1e6f,0,0});
    require(intersectsFrustum(extractFrustum(shifted),{{1e6f-.1f,-.1f,.4f},{1e6f+.1f,.1f,.6f},true}),
            "Large-coordinate box rejected");
    Aabb receiver{{-.1f,-.1f,-1.2f},{.1f,.1f,-.8f},true};
    require(intersectsDepthRange(receiver,{0,0,0},{0,0,-1},1.2f,2),"Depth boundary contact rejected");
    require(!intersectsDepthRange(receiver,{0,0,0},{0,0,-1},2,4),"Unused receiver interval retained");
    require(intersectsDepthRange(Aabb{}, {0,0,0},{0,0,-1},2,4),"Unknown receiver bounds rejected");
    require(intersectsDepthRange(receiver,{0,0,0},{0,0,0},2,4),"Invalid view direction rejected");
    std::cout<<"PASS Vulkan frustum boundaries, perspective, guards and conservative invalid inputs\n";
  } catch(std::exception const &error) {std::cerr<<error.what()<<'\n';return 1;}
}
