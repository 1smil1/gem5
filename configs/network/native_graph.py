"""Compact inputs for the existing Garnet C++ objects; no routing policy here."""

from m5.objects import GarnetNetwork, GarnetNativeGraph, GarnetRouter
from m5.params import Bool, isNullPointer
from m5.proxy import Parent, isproxy

ROUTER_FIELDS = frozenset({"router_id", "latency", "width", "role", "vcs_per_vnet", "virt_nets", "clk_domain", "eventq_index"})
LINK_FIELDS = frozenset({
    "link_id", "src_node", "dst_node", "int_node", "ext_node", "clk_domain",
    "latency", "serdes_latency", "is_off_chip", "weight", "bandwidth_factor", "width",
    "supported_vnets", "src_cdc", "dst_cdc", "src_serdes", "dst_serdes",
    "ext_cdc", "int_cdc", "ext_serdes", "int_serdes", "onchip_component_latency",
    "offchip_component_latency", "serdes_component_latency", "native_bridge_serdes_latency",
    "native_bridge_cdc_latency", "native_link_clock", "native_vcs", "native_vnets", "eventq_index",
    "src_outport", "dst_inport", "ext_net_bridge", "ext_cred_bridge", "int_net_bridge", "int_cred_bridge",
})


def _label(field, index, count):
    return field if count == 1 else f"{field}{index:0{len(str(count - 1))}d}"


def _text(value):
    value = str(value)
    if any(character in value for character in "\t\r\n"):
        raise ValueError("Native topology field contains a record delimiter")
    return value


class Record:
    __slots__ = ("_class", "_values", "_anchor", "_owner", "_name", "_parent_record", "_parent_field", "_anchor_class", "_original_clock")

    def __init__(self, cls, **parameters):
        object.__setattr__(self, "_class", cls)
        object.__setattr__(self, "_values", {})
        object.__setattr__(self, "_anchor", None)
        object.__setattr__(self, "_owner", None)
        object.__setattr__(self, "_name", None)
        object.__setattr__(self, "_parent_record", None)
        object.__setattr__(self, "_parent_field", None)
        object.__setattr__(self, "_anchor_class", cls)
        object.__setattr__(self, "_original_clock", None)
        for name, value in parameters.items():
            setattr(self, name, value)

    def __getattr__(self, name):
        if name == "_children":
            return {} if self._anchor is None else self._anchor._children
        if name in self._values:
            return self._values[name]
        if self._anchor is not None:
            return getattr(self._anchor, name)
        # These are the original gem5 declaration defaults, not missing
        # computed results. A required field without a declaration raises.
        return self._class._values[name]

    def __setattr__(self, name, value):
        if name.startswith("_"):
            object.__setattr__(self, name, value)
            return
        if name not in (ROUTER_FIELDS if isinstance(self, RouterRecord) else LINK_FIELDS):
            raise ValueError(f"Native topology does not encode parameter {name}")
        if name in ("src_node", "dst_node", "int_node"):
            if not isinstance(value, RouterRecord):
                raise TypeError(f"{name} requires a native router record")
            converted = value
            if value._parent_record is None:
                object.__setattr__(value, "_parent_record", self)
                object.__setattr__(value, "_parent_field", name)
        elif name == "clk_domain" and name not in self._class._params:
            converted = GarnetRouter._params["clk_domain"].convert(value)
        else:
            converted = self._class._params[name].convert(value)
        self._values[name] = converted
        if self._anchor is not None:
            setattr(self._anchor, name,
                    converted.anchor() if isinstance(converted, RouterRecord) else converted)

    def getCCObject(self):
        if self._owner is None or self._name is None:
            raise RuntimeError("Native topology record has not been sealed")
        return self._owner.native_graph.getCCObject().findChild(self._name)

    def anchor(self):
        if self._anchor is None:
            parameters = {}
            for name, value in self._values.items():
                if isinstance(value, RouterRecord):
                    actual = value.anchor()
                    if value._parent_record is not self:
                        value._parent_record.anchor()
                    parameters[name] = actual
                else:
                    parameters[name] = value
            if self._anchor_class is not self._class:
                parameters["clk_domain"] = self._original_clock
            object.__setattr__(self, "_anchor", self._anchor_class(**parameters))
        return self._anchor


class RouterRecord(Record):
    __slots__ = ()

    def __setattr__(self, name, value):
        super().__setattr__(name, value)
        if name == "clk_domain" and not value.has_parent():
            self.anchor()

    def anchor(self):
        if self._anchor is None:
            object.__setattr__(self, "_anchor", self._class(**self._values))
        return self._anchor


def _skip_python_lifecycle():
    # The same C++ virtual callbacks run in NativeGraph's ordered subtree.
    pass


class NativeGraphNetwork(GarnetNetwork):
    def __setattr__(self, name, value):
        if name in ("routers", "int_links", "ext_links"):
            object.__setattr__(self, "_records_" + name, list(value))
            super().__setattr__(name, [])
        else:
            super().__setattr__(name, value)

    def __getattr__(self, name):
        if name in ("routers", "int_links", "ext_links"):
            return object.__getattribute__(self, "_records_" + name)
        return super().__getattr__(name)

    def makeInterfaceAnchor(self, cls):
        if not self.ext_links:
            raise ValueError("Native topology requires external links")
        anchor = cls(id=0)
        self.add_child(_label("netifs", 0, len(self.ext_links)), anchor)
        object.__setattr__(self, "_interface_anchor", anchor)

    def adoptOrphanParams(self):
        if isNullPointer(self.native_graph):
            self._seal_graph()
        super().adoptOrphanParams()

    def _seal_graph(self):
        clocks = []
        clock_indices = {}
        router_anchors = []
        ext_anchors = []
        int_anchors = []
        lifecycle = []
        endpoints = []
        lines = [f"GARNET_GRAPH\t1\t{len(self.routers)}\t{len(self.int_links)}\t{len(self.ext_links)}"]

        def scalar(record, name):
            value = getattr(record, name)
            if isproxy(value):
                declaration = record._class._values[name]
                if value is not declaration:
                    raise ValueError(f"Unsupported explicit native topology proxy: {name}")
                if name in ("vcs_per_vnet", "native_vcs"):
                    value = self.vcs_per_vnet
                elif name in ("virt_nets", "native_vnets"):
                    value = self.number_of_virtual_networks
                elif name == "eventq_index":
                    value = (scalar(record._parent_record, name)
                             if record._parent_record is not None else self.eventq_index)
                    if isproxy(value):
                        value = value.unproxy(self)
                else:
                    raise ValueError(f"Unresolved native topology scalar: {name}")
            return int(bool(value)) if isinstance(value, Bool) else int(value)

        def clock_index(clock):
            key = id(clock)
            if key not in clock_indices:
                clock_indices[key] = len(clocks)
                clocks.append(clock)
            return clock_indices[key]

        def supported(record):
            values = record.supported_vnets
            if isproxy(values):
                raise ValueError("Unresolved supported_vnets in native topology")
            return ",".join(str(int(value)) for value in values)

        def anchor_object(obj, name):
            self.add_child(name, obj)
            subtree = list(obj.descendants())
            lifecycle.extend(subtree)
            for child in subtree:
                for method in ("init", "initState", "regProbePoints", "regProbeListeners", "startup"):
                    object.__setattr__(child, method, _skip_python_lifecycle)

        def own_record(record, field, index, count):
            name = _label(field, index, count)
            object.__setattr__(record, "_owner", self)
            object.__setattr__(record, "_name", self.path() + "." + name)
            return name

        for field in ("int_links", "ext_links"):
            records = getattr(self, field)
            for index, record in enumerate(records):
                own_record(record, field, index, len(records))

        for index, record in enumerate(self.routers):
            if record._parent_record is None:
                raise ValueError(f"Native router {index} has no original owner")
            relative = record._parent_record._name[len(self.path()) + 1:] + "." + record._parent_field
            object.__setattr__(record, "_owner", self)
            object.__setattr__(record, "_name", self.path() + "." + relative)
            anchor = -1
            if record._anchor is not None:
                anchor = len(router_anchors)
                router_anchors.append(record._anchor)
            lines.append("\t".join(map(str, [
                "R", scalar(record, "router_id"), scalar(record, "latency"),
                scalar(record, "width"), scalar(record, "vcs_per_vnet"),
                scalar(record, "virt_nets"), clock_index(record.clk_domain), anchor,
                scalar(record, "eventq_index"), _text(record.role), _text(relative),
            ])))

        common = ("latency", "serdes_latency", "is_off_chip", "weight", "bandwidth_factor", "width")
        for index, record in enumerate(self.int_links):
            own_record(record, "int_links", index, len(self.int_links))
            anchor = -1
            if record._anchor is not None:
                anchor = len(int_anchors)
                int_anchors.append(record._anchor)
                anchor_object(record._anchor, _label("int_links", index, len(self.int_links)))
            lines.append("\t".join(map(str, [
                "I", scalar(record, "link_id"), int(record.src_node.router_id), int(record.dst_node.router_id),
                *(scalar(record, name) for name in common),
                *(scalar(record, name) for name in ("src_cdc", "dst_cdc", "src_serdes", "dst_serdes")),
                *(scalar(record, name) for name in ("onchip_component_latency", "offchip_component_latency", "serdes_component_latency")),
                scalar(record, "native_bridge_serdes_latency"), scalar(record, "native_bridge_cdc_latency"),
                clock_index(record.native_link_clock), scalar(record, "eventq_index"), supported(record),
                _text(record.src_outport), _text(record.dst_inport), scalar(record, "native_vcs"), scalar(record, "native_vnets"),
                anchor,
            ])))

        for index, record in enumerate(self.ext_links):
            name = own_record(record, "ext_links", index, len(self.ext_links))
            anchor = -1
            if record._anchor is not None:
                anchor = len(ext_anchors)
                ext_anchors.append(record._anchor)
                anchor_object(record._anchor, name)
            endpoints.append(record.ext_node)
            link_clock = Parent.clk_domain if record._anchor is not None else record.native_link_clock
            lines.append("\t".join(map(str, [
                "E", scalar(record, "link_id"), index, int(record.int_node.router_id),
                *(scalar(record, name) for name in common),
                *(scalar(record, name) for name in ("ext_cdc", "int_cdc", "ext_serdes", "int_serdes")),
                clock_index(link_clock), scalar(record, "eventq_index"), supported(record),
                *(scalar(record, name) for name in ("onchip_component_latency", "offchip_component_latency", "serdes_component_latency")),
                anchor, scalar(record, "native_vcs"), scalar(record, "native_vnets"),
            ])))

        # It already has its canonical parent/name; suppress its Python
        # callback only after collecting all its actual descendant objects.
        lifecycle.extend(self._interface_anchor.descendants())
        for child in self._interface_anchor.descendants():
            for method in ("init", "initState", "regProbePoints", "regProbeListeners", "startup"):
                object.__setattr__(child, method, _skip_python_lifecycle)
        self.native_graph = GarnetNativeGraph(
            records="\n".join(lines), clocks=clocks, endpoints=endpoints,
            router_anchors=router_anchors, ext_anchors=ext_anchors,
            int_anchors=int_anchors,
            ni_anchor=self._interface_anchor, lifecycle_anchors=lifecycle,
            power_template=Parent.online_power_state_template,
        )
        aliases = {}
        for name in self._children:
            alias = name
            for field in ("int_links", "ext_links", "netifs", "routers"):
                suffix = name[len(field):] if name.startswith(field) else ""
                if suffix.isdecimal():
                    alias = field + str(int(suffix))
            aliases[name] = alias
        object.__setattr__(self, "_native_stat_group_labels", aliases)


def factories(native_internal, native_external, legacy_external):
    def router(**parameters):
        return RouterRecord(GarnetRouter, **parameters)

    def internal(**parameters):
        clock = parameters.pop("clk_domain")
        if not clock.has_parent():
            # Keep an actual clock-owning router rather than orphaning clocks
            # through a lightweight link record.
            raise ValueError("Native graph internal link clock has no owner")
        record = Record(native_internal, **parameters)
        if any(value._anchor is not None and value._parent_record is record
               for value in (record.src_node, record.dst_node)):
            record.anchor()
        return record

    def external(**parameters):
        clock = parameters.pop("clk_domain")
        if not clock.has_parent():
            raise ValueError("Native graph external link clock has no owner")
        record = Record(native_external, **parameters)
        bridged = any(bool(getattr(record, name)) for name in ("ext_cdc", "int_cdc", "ext_serdes", "int_serdes"))
        if bridged:
            # Preserve the original bridged parent and its original children;
            # this is an explicit ownership choice, not a numerical fallback.
            object.__setattr__(record, "_anchor_class", legacy_external)
            object.__setattr__(record, "_original_clock", clock)
            record.anchor()
        elif record.int_node._anchor is not None and record.int_node._parent_record is record:
            record.anchor()
        return record

    return router, internal, external
