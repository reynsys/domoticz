#pragma once

#include "DelayedLink.h"
#include "Plugins.h"

#ifndef byte
typedef unsigned char byte;
#endif

namespace Plugins {

	extern std::mutex PythonMutex;			// controls access to Python
	extern struct PyModuleDef DomoticzExModuleDef;

	class CPluginMessageBase
	{
	public:
	  virtual ~CPluginMessageBase() = default;
	  ;

	  CPlugin *m_pPlugin;
	  std::string m_Name;
	  int m_HwdID;
	  std::string m_DeviceID;
	  int m_Unit;
	  bool m_Delay;
	  time_t m_When;

	protected:
		CPluginMessageBase(CPlugin* pPlugin) : m_pPlugin(pPlugin), m_HwdID(pPlugin->m_HwdID), m_Unit(-1), m_Delay(false)
		{
			m_Name = __func__;
			m_When = time(nullptr);
		};
		virtual void ProcessLocked() = 0;
	public:
		virtual const char* Name() { return m_Name.c_str(); };
		virtual const CPlugin*	Plugin() { return m_pPlugin; };
		virtual void Process()
		{
			std::lock_guard<std::mutex> l(PythonMutex);
			m_pPlugin->RestoreThread();
			ProcessLocked();
			m_pPlugin->ReleaseThread();
		};
	};

	// Handles lifecycle management of the Python Connection object
	class CHasConnection
	{
	public:
		CHasConnection(CConnection *Connection) : m_pConnection(Connection)
		{
			Py_XINCREF(m_pConnection);
		};
		~CHasConnection()
		{
			Py_XDECREF(m_pConnection);
		}
		CConnection *m_pConnection;
	};

	class InitializeMessage : public CPluginMessageBase
	{
	public:
		InitializeMessage(CPlugin* pPlugin) : CPluginMessageBase(pPlugin) { m_Name = __func__; };
		void Process() override
		{
			std::lock_guard<std::mutex> l(PythonMutex);
			m_pPlugin->Initialise();
		};
		void ProcessLocked() override{};
	};

	// Base callback message class
	class CCallbackBase : public CPluginMessageBase
	{
	protected:
		PyNewRef	m_Target;
		std::string m_Callback;
		void ProcessLocked() override = 0;

	      public:
		CCallbackBase(CPlugin* pPlugin, const std::string &Callback) : CPluginMessageBase(pPlugin), m_Target(pPlugin->PythonModule()), m_Callback(Callback)
		{
			if (m_Target)
			{
				Py_INCREF(m_Target);
			}
		};
		virtual void Callback(PyObject* pParams) { if (m_Callback.length()) m_pPlugin->Callback(m_Target, m_Callback, pParams); };
		virtual const char* PythonName() { return m_Callback.c_str(); };

	      protected:
		bool UpdateEventTarget(const std::string DeviceID, int Unit)
		{
			// Used by some events in DomoticzEx.  Looks for callback existance on Unit, Device and Plugin in that order
			PyBorrowedRef pModule = PyState_FindModule(&DomoticzExModuleDef);
			if (pModule)
			{
				module_state *pModState = ((struct module_state *)PyModule_GetState(pModule));
				if (pModState)
				{
					PyBorrowedRef	pUnit = pModState->pPlugin->FindUnitInDevice(m_DeviceID, m_Unit);
					if ((pUnit) && (PyObject_HasAttrString(pUnit, m_Callback.c_str())))
					{
						PyNewRef pFunc = PyObject_GetAttrString(pUnit, m_Callback.c_str());
						if (pFunc && PyCallable_Check(pFunc))
						{
							m_Target += pUnit;	// Increment ref count because pFunc will decrement it on function exit
							return true;
						}
					}

					PyBorrowedRef	pDevice = pModState->pPlugin->FindDevice(m_DeviceID);
					if ((pDevice) && (PyObject_HasAttrString(pDevice, m_Callback.c_str())))
					{
						PyNewRef pFunc = PyObject_GetAttrString(pDevice, m_Callback.c_str());
						if (pFunc && PyCallable_Check(pFunc))
						{
							m_Target += pDevice;	// Increment ref count because pFunc will decrement it on function exit
							return true;
						}
					}
				}
				// True signals that DomoticzEx is loaded so both DeviceID and Unit should be used in the callback
				return true;
			}
			return false;
		}
	};

	class onStartCallback : public CCallbackBase
	{
	public:
		onStartCallback(CPlugin* pPlugin) : CCallbackBase(pPlugin, "onStart") { m_Name = __func__; };
	protected:
	  void ProcessLocked() override
	  {
		  m_pPlugin->Start();
		  Callback(nullptr);
	  };
	};

	class onHeartbeatCallback : public CCallbackBase
	{
	public:
		onHeartbeatCallback(CPlugin* pPlugin) : CCallbackBase(pPlugin, "onHeartbeat") { m_Name = __func__; };
	protected:
	  void ProcessLocked() override
	  {
		  Callback(nullptr);
	  };
	};

#ifdef _WIN32
static std::wstring string_to_wstring(const std::string &str, int codepage)
{
	if (str.empty()) return std::wstring();
	int sz = MultiByteToWideChar(codepage, 0, &str[0], (int)str.size(), 0, 0);
	std::wstring res(sz, 0);
	MultiByteToWideChar(codepage, 0, &str[0], (int)str.size(), &res[0], sz);
	return res;
}

static std::string wstring_to_string(const std::wstring &wstr, int codepage)
{
	if (wstr.empty()) return std::string();
	int sz = WideCharToMultiByte(codepage, 0, &wstr[0], (int)wstr.size(), 0, 0, 0, 0);
	std::string res(sz, 0);
	WideCharToMultiByte(codepage, 0, &wstr[0], (int)wstr.size(), &res[0], sz, 0, 0);
	return res;
}

static std::string get_utf8_from_ansi(const std::string &utf8, int codepage)
{
	std::wstring utf16 = string_to_wstring(utf8, codepage);
	std::string ansi = wstring_to_string(utf16, CP_UTF8);
	return ansi;
}
#endif

	class onConnectCallback : public CCallbackBase, public CHasConnection
	{
	public:
		onConnectCallback(CPlugin* pPlugin, CConnection* Connection, const int Code, const std::string &Text) : CCallbackBase(pPlugin, "onConnect"), CHasConnection(Connection), m_Status(Code), m_Text(Text)
		{
			m_Name = __func__;
			if (Connection->Target)
			{
				m_Target += Connection->Target;
			}
		};
		int						m_Status;
		std::string				m_Text;
	protected:
	  void ProcessLocked() override
	  {
#ifdef _WIN32
			std::string textUTF8 = get_utf8_from_ansi(m_Text, GetACP());
#else
			std::string textUTF8 = m_Text; // TODO: Is it safe to assume non-Windows will always be UTF-8?
#endif
			PyNewRef pParams = Py_BuildValue("Ois", m_pConnection, m_Status, textUTF8.c_str());
			Callback(pParams);
	  };
	};

	class onTimeoutCallback : public CCallbackBase, public CHasConnection
	{
    public:
		onTimeoutCallback(CPlugin *pPlugin, CConnection *Connection): CCallbackBase(pPlugin, "onTimeout"), CHasConnection(Connection) 
		{
			m_Name = __func__;
			if (Connection->Target)
			{
				m_Target += Connection->Target;
			}
		};
    protected:
		virtual void ProcessLocked() override
		{
			PyNewRef pParams = Py_BuildValue("(O)", m_pConnection);
			Callback(pParams);
		};
	};

	class onDisconnectCallback : public CCallbackBase, public CHasConnection
	{
	public:
		onDisconnectCallback(CPlugin* pPlugin, CConnection* Connection) : CCallbackBase(pPlugin, "onDisconnect"), CHasConnection(Connection)
		{
			m_Name = __func__;
			if (Connection->Target)
			{
				m_Target += Connection->Target;
			}
		};
	protected:
	  void ProcessLocked() override
	  {
		  PyNewRef pParams = Py_BuildValue("(O)", m_pConnection);
		  Callback(pParams);
	  };
	};

	class onDeviceAddedCallback : public CCallbackBase
	{
	public:
		onDeviceAddedCallback(CPlugin *pPlugin, const std::string DeviceID, int Unit) : CCallbackBase(pPlugin, "onDeviceAdded")
		{
			m_DeviceID = DeviceID;
			m_Unit = Unit;
		};
	protected:
		void ProcessLocked() override
		{
			m_pPlugin->onDeviceAdded(m_DeviceID, m_Unit);

			if (UpdateEventTarget(m_DeviceID, m_Unit))
			{
				if (CUnitEx::isInstance(m_Target))
				{
					PyNewRef pParams = Py_BuildValue("()");
					Callback(pParams);
				}
				else if (CDeviceEx::isInstance(m_Target))
				{
					PyNewRef pParams = Py_BuildValue("(i)", m_Unit);
					Callback(pParams);
				}
				else
				{
					PyNewRef pParams = Py_BuildValue("(si)", m_DeviceID.c_str(), m_Unit);
					Callback(pParams);
				}
			}
			else
			{
				PyNewRef pParams = Py_BuildValue("(i)", m_Unit);
				Callback(pParams);
			}
		};
	};

	class onDeviceModifiedCallback : public CCallbackBase
	{
	public:
		onDeviceModifiedCallback(CPlugin *pPlugin, const std::string DeviceID, int Unit) : CCallbackBase(pPlugin, "onDeviceModified")
		{
			m_DeviceID = DeviceID;
			m_Unit = Unit;
		};
	protected:
		void ProcessLocked() override
		{
			m_pPlugin->onDeviceModified(m_DeviceID, m_Unit);

			if (UpdateEventTarget(m_DeviceID, m_Unit))
			{
				if (CUnitEx::isInstance(m_Target))
				{
					PyNewRef pParams = Py_BuildValue("()");
					Callback(pParams);
				}
				else if (CDeviceEx::isInstance(m_Target))
				{
					PyNewRef pParams = Py_BuildValue("(i)", m_Unit);
					Callback(pParams);
				}
				else
				{
					PyNewRef pParams = Py_BuildValue("(si)", m_DeviceID.c_str(), m_Unit);
					Callback(pParams);
				}
			}
			else
			{
				PyNewRef pParams = Py_BuildValue("(i)", m_Unit);
				Callback(pParams);
			}
		};
	};

	class onDeviceRemovedCallback : public CCallbackBase
	{
	public:
		onDeviceRemovedCallback(CPlugin *pPlugin, const std::string DeviceID, int Unit) : CCallbackBase(pPlugin, "onDeviceRemoved")
		{
			m_DeviceID = DeviceID;
			m_Unit = Unit;
		};
	protected:
	  void ProcessLocked() override
	  {
		  if (UpdateEventTarget(m_DeviceID, m_Unit))
		  {
			  if (CUnitEx::isInstance(m_Target))
			  {
				  PyNewRef pParams = Py_BuildValue("()");
				  Callback(pParams);
			  }
			  else if (CDeviceEx::isInstance(m_Target))
			  {
				  PyNewRef pParams = Py_BuildValue("(i)", m_Unit);
				  Callback(pParams);
			  }
			  else
			  {
				  PyNewRef pParams = Py_BuildValue("(si)", m_DeviceID.c_str(), m_Unit);
				  Callback(pParams);
			  }
		  }
		  else
		  {
			  PyNewRef pParams = Py_BuildValue("(i)", m_Unit);
			  Callback(pParams);
		  }

		  m_pPlugin->onDeviceRemoved(m_DeviceID, m_Unit);
	  };
	};

	class onCommandCallback : public CCallbackBase
	{
	public:
		onCommandCallback(CPlugin *pPlugin, const std::string DeviceID, int Unit, const std::string &Command, const int level, const std::string &color)
			: CCallbackBase(pPlugin, "onCommand")
		{
			m_Name = __func__;
			m_DeviceID = DeviceID;
			m_Unit = Unit;
			m_fLevel = -273.15F;
			m_Command = Command;
			m_iLevel = level;
			m_iColor = color;
		};
		onCommandCallback(CPlugin *pPlugin, const std::string DeviceID, int Unit, const std::string &Command, const float level)
			: CCallbackBase(pPlugin, "onCommand")
		{
			m_Name = __func__;
			m_DeviceID = DeviceID;
			m_Unit = Unit;
			m_fLevel = level;
			m_Command = Command;
			m_iLevel = -1;
			m_iColor = "";
		};
		std::string				m_Command;
		std::string				m_iColor;
		int						m_iLevel;
		float					m_fLevel;

	protected:
	  void ProcessLocked() override
	  {
		  PyNewRef pParams;
		  if (UpdateEventTarget(m_DeviceID, m_Unit))
		  {
			  if (CUnitEx::isInstance(m_Target))
			  {
				  if (m_fLevel != -273.15F)
				  {
					  pParams = Py_BuildValue("sfs", m_Command.c_str(), m_fLevel, "");
				  }
				  else
				  {
					  pParams = Py_BuildValue("sis", m_Command.c_str(), m_iLevel, m_iColor.c_str());
				  }
			  }
			  else if (CDeviceEx::isInstance(m_Target))
			  {
				  if (m_fLevel != -273.15F)
				  {
					  pParams = Py_BuildValue("isfs", m_Unit, m_Command.c_str(), m_fLevel, "");
				  }
				  else
				  {
					  pParams = Py_BuildValue("isis", m_Unit, m_Command.c_str(), m_iLevel, m_iColor.c_str());
				  }
			  }
			  else
			  {
				  if (m_fLevel != -273.15F)
				  {
					  pParams = Py_BuildValue("sisfs", m_DeviceID.c_str(), m_Unit, m_Command.c_str(), m_fLevel, "");
				  }
				  else
				  {
					  pParams = Py_BuildValue("sisis", m_DeviceID.c_str(), m_Unit, m_Command.c_str(), m_iLevel, m_iColor.c_str());
				  }
			  }
		  }
		  else
		  {
			  if (m_fLevel != -273.15F)
			  {
				  pParams = Py_BuildValue("isfs", m_Unit, m_Command.c_str(), m_fLevel, "");
			  }
			  else
			  {
				  pParams = Py_BuildValue("isis", m_Unit, m_Command.c_str(), m_iLevel, m_iColor.c_str());
			  }
		  }
		  Callback(pParams);
	  };
	};

	class onSecurityEventCallback : public CCallbackBase
	{
	public:
		onSecurityEventCallback(CPlugin *pPlugin, const std::string DeviceID, int Unit, const int level, const std::string &Description)
			: CCallbackBase(pPlugin, "onSecurityEvent")
		{
			m_Name = __func__;
			m_DeviceID = DeviceID;
			m_Unit = Unit;
			m_iLevel = level;
			m_Description = Description;
		};
		int				m_iLevel;
		std::string		m_Description;

	protected:
	  void ProcessLocked() override
	  {
		  if (UpdateEventTarget(m_DeviceID, m_Unit))
		  {
			  if (CUnitEx::isInstance(m_Target))
			  {
				  PyNewRef pParams = Py_BuildValue("is", m_iLevel, m_Description.c_str());
				  Callback(pParams);
			  }
			  else if (CDeviceEx::isInstance(m_Target))
			  {
				  PyNewRef pParams = Py_BuildValue("iis", m_Unit, m_iLevel, m_Description.c_str());
				  Callback(pParams);
			  }
			  else
			  {
				  PyNewRef pParams = Py_BuildValue("siis", m_DeviceID.c_str(), m_Unit, m_iLevel, m_Description.c_str());
				  Callback(pParams);
			  }
		  }
		  else
		  {
			  PyNewRef pParams = Py_BuildValue("iis", m_Unit, m_iLevel, m_Description.c_str());
			  Callback(pParams);
		  }
	  };
	};

	class onMessageCallback : public CCallbackBase, public CHasConnection
	{
	public:
		onMessageCallback(CPlugin *pPlugin, CConnection *Connection, const std::string &Buffer) : CCallbackBase(pPlugin, "onMessage"), CHasConnection(Connection), m_Data(nullptr)
		{
			m_Name = __func__;
			m_Buffer.reserve(Buffer.length());
			m_Buffer.assign((const byte *)Buffer.c_str(), (const byte *)Buffer.c_str() + Buffer.length());
			if (Connection->Target)
			{
				m_Target += Connection->Target;
			}
		};
		onMessageCallback(CPlugin *pPlugin, CConnection *Connection, const std::vector<byte> &Buffer): CCallbackBase(pPlugin, "onMessage"), CHasConnection(Connection), m_Data(nullptr)
		{
			m_Name = __func__;
			m_Buffer = Buffer;
			if (Connection->Target)
			{
				m_Target += Connection->Target;
			}
		};
		onMessageCallback(CPlugin* pPlugin, CConnection* Connection, PyObject*	pData) : CCallbackBase(pPlugin, "onMessage"), CHasConnection(Connection)
		{
			m_Name = __func__;
			m_Data = pData;
			if (Connection->Target)
			{
				m_Target += Connection->Target;
			}
		};
		std::vector<byte>		m_Buffer;
		PyObject*				m_Data;

	protected:
	  void ProcessLocked() override
	  {
		  PyNewRef pParams = nullptr;

		  // Data is stored in a single vector of bytes
		  if (!m_Buffer.empty())
		  {
			  pParams = Py_BuildValue("Oy#", m_pConnection, &m_Buffer[0], m_Buffer.size());
			  Callback(pParams);
		  }

		  // Data is in a dictionary
		  if (m_Data)
		  {
			  pParams = Py_BuildValue("OO", m_pConnection, m_Data);
			  Callback(pParams);
			  Py_XDECREF(m_Data);
		  }
	  }
	};

	class onNotificationCallback : public CCallbackBase
	{
	public:
		onNotificationCallback(CPlugin* pPlugin,
							const std::string& Subject,
							const std::string& Text,
							const std::string& Name,
							const std::string& Status,
							int Priority,
							const std::string& Sound,
							const std::string& ImageFile) : CCallbackBase(pPlugin, "onNotification")
		{
			m_Name = __func__;
			m_Subject = Subject;
			m_Text = Text;
			m_SuppliedName = Name;
			m_Status = Status;
			m_Priority = Priority;
			m_Sound = Sound;
			m_ImageFile = ImageFile;
		};

		std::string				m_Subject;
		std::string				m_Text;
		std::string				m_SuppliedName;
		std::string				m_Status;
		int						m_Priority;
		std::string				m_Sound;
		std::string				m_ImageFile;

	protected:
	  void ProcessLocked() override
	  {
		  PyNewRef pParams = Py_BuildValue("ssssiss", m_SuppliedName.c_str(), m_Subject.c_str(), m_Text.c_str(), m_Status.c_str(), m_Priority, m_Sound.c_str(), m_ImageFile.c_str());
		  Callback(pParams);
	  };
	};

	class onStopCallback : public CCallbackBase
	{
	public:
		onStopCallback(CPlugin* pPlugin) : CCallbackBase(pPlugin, "onStop") { m_Name = __func__; };
	protected:
	  void ProcessLocked() override
	  {
		  Callback(nullptr);
		  m_pPlugin->Stop();
	  };
	};

	// Base directive message class
	class CDirectiveBase : public CPluginMessageBase
	{
	protected:
	  void ProcessLocked() override = 0;

	public:
		CDirectiveBase(CPlugin* pPlugin) : CPluginMessageBase(pPlugin) {};
		void Process() override
		{
			std::lock_guard<std::mutex> l(PythonMutex);
			m_pPlugin->RestoreThread();
			ProcessLocked();
			m_pPlugin->ReleaseThread();
		};
	};

	class ProtocolDirective : public CDirectiveBase, public CHasConnection
	{
	public:
		ProtocolDirective(CPlugin* pPlugin, CConnection* Connection) : CDirectiveBase(pPlugin), CHasConnection(Connection) { m_Name = __func__; };
		void ProcessLocked() override
		{
			m_pPlugin->ConnectionProtocol(this);
		};
	};

	class ConnectDirective : public CDirectiveBase, public CHasConnection
	{
	public:
		ConnectDirective(CPlugin* pPlugin, CConnection* Connection) : CDirectiveBase(pPlugin), CHasConnection(Connection) { m_Name = __func__; };
		void ProcessLocked() override
		{
			m_pPlugin->ConnectionConnect(this);
		};
	};

	class ListenDirective : public CDirectiveBase, public CHasConnection
	{
	public:
		ListenDirective(CPlugin* pPlugin, CConnection* Connection) : CDirectiveBase(pPlugin), CHasConnection(Connection) { m_Name = __func__; };
		void ProcessLocked() override
		{
			m_pPlugin->ConnectionListen(this);
		};
	};

	class DisconnectDirective : public CDirectiveBase, public CHasConnection
	{
	public:
		DisconnectDirective(CPlugin* pPlugin, CConnection* Connection) : CDirectiveBase(pPlugin), CHasConnection(Connection) { m_Name = __func__; };
		void ProcessLocked() override
		{
			m_pPlugin->ConnectionDisconnect(this);
		};
	};

	class WriteDirective : public CDirectiveBase, public CHasConnection
	{
	public:
		PyObject*		m_Object;
		WriteDirective(CPlugin* pPlugin, CConnection* Connection, PyObject* pData, const int Delay) : CDirectiveBase(pPlugin), CHasConnection(Connection)
		{
			m_Name = __func__;
			m_Object = pData;
			if (m_Object)
				Py_INCREF(m_Object);
			if (Delay)
			{
				m_When += Delay;
				m_Delay=true;
			}
		};
		~WriteDirective() override
		{
			if (m_Object)
				Py_DECREF(m_Object);
		}

		void ProcessLocked() override
		{
			m_pPlugin->ConnectionWrite(this);
		};
	};

	class SettingsDirective : public CDirectiveBase
	{
	public:
		SettingsDirective(CPlugin* pPlugin) : CDirectiveBase(pPlugin) { m_Name = __func__; };
		void ProcessLocked() override
		{
			m_pPlugin->LoadSettings();
		};
	};

	class PollIntervalDirective : public CDirectiveBase
	{
	public:
		PollIntervalDirective(CPlugin* pPlugin, const int PollInterval) : CDirectiveBase(pPlugin), m_Interval(PollInterval) { m_Name = __func__; };
		int						m_Interval;
		void ProcessLocked() override
		{
			m_pPlugin->PollInterval(m_Interval);
		};
	};

	class NotifierDirective : public CDirectiveBase
	{
	public:
		NotifierDirective(CPlugin* pPlugin, const char* Name) : CDirectiveBase(pPlugin), m_NotifierName(Name) { m_Name = __func__; };
		std::string		m_NotifierName;
		void ProcessLocked() override
		{
			m_pPlugin->Notifier(m_NotifierName);
		};
	};

	// Base event message class
	class CEventBase : public CPluginMessageBase
	{
	protected:
	  void ProcessLocked() override = 0;

	public:
		CEventBase(CPlugin* pPlugin) : CPluginMessageBase(pPlugin) {};
	};

	class ReadEvent : public CEventBase, public CHasConnection
	{
	public:
		ReadEvent(CPlugin* pPlugin, CConnection* Connection, const int ByteCount, const unsigned char* Data, const int ElapsedMs = -1) : CEventBase(pPlugin), CHasConnection(Connection)
		{
			m_Name = __func__;
			m_ElapsedMs = ElapsedMs;
			m_Buffer.reserve(ByteCount);
			m_Buffer.assign(Data, Data + ByteCount);
		};
		std::vector<byte>		m_Buffer;
		int						m_ElapsedMs;
		void ProcessLocked() override
		{
			m_pPlugin->WriteDebugBuffer(m_Buffer, true);
			m_pPlugin->ConnectionRead(this);
		};
	};

	class DisconnectedEvent : public CEventBase, public CHasConnection
	{
	public:
		DisconnectedEvent(CPlugin* pPlugin, CConnection* Connection) : CEventBase(pPlugin), CHasConnection(Connection), bNotifyPlugin(true) { m_Name = __func__; };
		DisconnectedEvent(CPlugin* pPlugin, CConnection* Connection, bool NotifyPlugin) : CEventBase(pPlugin), CHasConnection(Connection), bNotifyPlugin(NotifyPlugin) { m_Name = __func__; };
		void ProcessLocked() override
		{
			m_pPlugin->DisconnectEvent(this);
		};
		bool	bNotifyPlugin;
	};
} // namespace Plugins
