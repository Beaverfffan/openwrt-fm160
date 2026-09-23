'use strict';
'require network';

// FM160 QMAP 拨号协议（广和通 vendor CLI 实现，非原生 uqmi）。
// 拨号参数（APN / 协议栈 / QMAP 通道数）由「服务 → FM160 → 拨号」页
// 统一管理，这里只负责让接口在 LuCI 正常显示/编辑。
return network.registerProtocol('qmi_fm160', {
	getI18n: function() {
		return _('FM160 QMAP');
	},

	getIfname: function() {
		return this._ubus('l3_device') || 'wwan0';
	},

	getPackageName: function() {
		return 'luci-proto-qmi-fm160';
	},

	isFloating: function() {
		return true;
	},

	isVirtual: function() {
		return true;
	},

	getDevices: function() {
		return null;
	},

	containsDevice: function(ifname) {
		return (network.getIfnameOf(ifname) == this.getIfname());
	},

	renderFormOptions: function(s) {
		// APN / v4 / v6 / QMAP 通道数 / 自动拨号在 FM160 拨号页配置
	}
});
