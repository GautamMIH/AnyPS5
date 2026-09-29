#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_BDARESOURCES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_BDARESOURCES_HPP

#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"

namespace AgcDriver::Graphics {

class BdaResources {
public:
    explicit BdaResources(const Context& context);
    BdaResources(const Context& context, const GuestBufferMemory& memory);
    VkDescriptorBufferInfo Table() const;
    VkDescriptorBufferInfo Fault() const;
    void CheckFault() const;

private:
    // The pages stores through the table noted in the fault buffer (BdaAbi::WrittenPageSlots) are
    // marked GPU-written, and the set emptied; CheckFault does it first.
    void markWrittenPages() const;
    std::unique_ptr<Buffer> table;
    std::unique_ptr<Buffer> fault;
    std::size_t tableBytes = 0;
};

}

#endif
