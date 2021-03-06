#ifndef _DATATYPE_DATAHEADER_H_
#define _DATATYPE_DATAHEADER_H_

#include <string>
#include <vector>
#include "../../Time/Time.h"
#include "DataBase.h"

namespace NS_DataType
{
  template<class ContainerAllocator>
    struct DataHeader_: public DataBase
    {
      typedef DataHeader_<ContainerAllocator> Type;
    public:
      DataHeader_ ()
          : seq (0), stamp (), frame_id ()
      {
      }
      ;

      DataHeader_ (const ContainerAllocator& allocator)
          : seq (0), stamp (allocator), frame_id (allocator)
      {
      }
      ;

      unsigned long seq;
      NS_NaviCommon::Time stamp;

      std::basic_string<char, std::char_traits<char>,
          typename ContainerAllocator::template rebind<char>::other> frame_id;

      typedef boost::shared_ptr<DataHeader_<ContainerAllocator> > Ptr;
      typedef boost::shared_ptr<DataHeader_<ContainerAllocator> const> ConstPtr;
    };
  
  typedef DataHeader_<std::allocator<void> > DataHeader;
  
  typedef boost::shared_ptr<DataHeader> DataHeaderPtr;
  typedef boost::shared_ptr<DataHeader const> DataHeaderConstPtr;

} /* namespace NS_NaviCommon */

#endif /* _DATAHEADER_H_ */
