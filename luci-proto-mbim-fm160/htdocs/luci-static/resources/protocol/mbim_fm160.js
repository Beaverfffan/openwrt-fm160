'use strict';
'require network';

// FM160 MBIM 拨号协议（广和通 vendor CLI 实现，非原生 umbim）。
// 拨号参数（APN / 协议栈）由「服务 → FM160 → 拨号」页统一管理，
// 这里只负责让接口在 LuCI 正常显示/编辑（防火墙区域、指标等）。
return network.registerProtocol('mbim_fm160', {
	getI18n: function() {
		return _('FM160 MBIM');
	},

	getIfname: function() {
		return this._ubus('l3_device') || 'wwan0';
	},

	getPackageName: function() {
		return 'luci-proto-mbim-fm160';
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
		// APN / v4 / v6 / 自动拨号在 FM160 拨号页配置
	}
});
