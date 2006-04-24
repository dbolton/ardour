#ifndef ardour_control_protocol_manager_h
#define ardour_control_protocol_manager_h

#include <string>
#include <list>

#include <sigc++/sigc++.h>

#include <pbd/lockmonitor.h>

#include <ardour/stateful.h>

namespace ARDOUR {

class ControlProtocol;
class ControlProtocolDescriptor;
class Session;

struct ControlProtocolInfo {
    ControlProtocolDescriptor* descriptor;
    ControlProtocol* protocol;
    std::string name;
    std::string path;
    bool requested;
};

 class ControlProtocolManager : public sigc::trackable, public Stateful
{
  public:
	ControlProtocolManager ();
	~ControlProtocolManager ();

	static ControlProtocolManager& instance() { return *_instance; }

	void set_session (Session&);
	void discover_control_protocols (std::string search_path);
	void foreach_known_protocol (sigc::slot<void,const ControlProtocolInfo*>);

	ControlProtocol* instantiate (ControlProtocolInfo&);
	int              teardown (ControlProtocolInfo&);

	std::list<ControlProtocolInfo*> control_protocol_info;

	static const std::string state_node_name;

	int set_state (const XMLNode&);
	XMLNode& get_state (void);

  private:
	static ControlProtocolManager* _instance;

	Session* _session;
	PBD::Lock protocols_lock;
	std::list<ControlProtocol*>    control_protocols;

	void drop_session ();

	int control_protocol_discover (std::string path);
	ControlProtocolDescriptor* get_descriptor (std::string path);
	ControlProtocolInfo* cpi_by_name (std::string);
};

} // namespace

#endif // ardour_control_protocol_manager_h
