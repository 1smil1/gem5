
import m5
from m5.objects import *
from m5.defines import buildEnv
from m5.util import addToPath
from m5.util.convert import toMemorySize
import os, argparse, json, sys, time

addToPath("../")

from common import Options
from ruby import Ruby

config_path = os.path.dirname(os.path.abspath(__file__))
config_root = os.path.dirname(config_path)
m5_root = os.path.dirname(config_root)

parser = argparse.ArgumentParser()
Options.addNoISAOptions(parser)

parser.add_argument(
    "--synthetic",
    default="uniform_random",
    choices=[
        "uniform_random",
        "tornado",
        "bit_complement",
        "bit_reverse",
        "bit_rotation",
        "neighbor",
        "shuffle",
        "transpose",
    ],
)

parser.add_argument(
    "-i",
    "--injectionrate",
    type=float,
    default=0.1,
    metavar="I",
    help="Injection rate in packets per cycle per node. \
                        Takes decimal value between 0 to 1 (eg. 0.225). \
                        Number of digits after 0 depends upon --precision.",
)

parser.add_argument(
    "--precision",
    type=int,
    default=3,
    help="Number of digits of precision after decimal point\
                        for injection rate",
)

parser.add_argument(
    "--sim-cycles", type=int, default=1000, help="Number of simulation cycles"
)

parser.add_argument(
    "--num-packets-max",
    type=int,
    default=-1,
    help="Stop injecting after --num-packets-max.\
                        Set to -1 to disable.",
)

parser.add_argument(
    "--single-sender-id",
    type=int,
    default=-1,
    help="Only inject from this sender.\
                        Set to -1 to disable.",
)

parser.add_argument(
    "--single-dest-id",
    type=int,
    default=-1,
    help="Only send to this destination.\
                        Set to -1 to disable.",
)

parser.add_argument(
    "--inj-vnet",
    type=int,
    default=-1,
    choices=[-1, 0, 1, 2],
    help="Only inject in this vnet (0, 1 or 2).\
                        0 and 1 are 1-flit, 2 is 5-flit.\
                        Set to -1 to inject randomly in all vnets.",
)

Ruby.define_options(parser)
parser.add_argument("--online-control", action="store_true")

args = parser.parse_args()

_init_started = time.monotonic()
def init_progress(stage):
    print(f"OMELET_INIT {stage}; wall_s={time.monotonic() - _init_started:.1f}", flush=True)

init_progress(f"creating traffic endpoints count={args.num_cpus}")
cpus = [
    GarnetSyntheticTraffic(
        num_packets_max=args.num_packets_max,
        single_sender=args.single_sender_id,
        single_dest=args.single_dest_id,
        sim_cycles=args.sim_cycles,
        traffic_type=args.synthetic,
        inj_rate=args.injectionrate,
        inj_vnet=args.inj_vnet,
        precision=args.precision,
        num_dest=args.num_dirs,
        memory_size=toMemorySize(args.mem_size),
        online_mode=args.online_control,
    )
    for i in range(args.num_cpus)
]

system = System(cpu=cpus, mem_ranges=[AddrRange(args.mem_size)])


system.voltage_domain = VoltageDomain(voltage=args.sys_voltage)

system.clk_domain = SrcClockDomain(
    clock=args.sys_clock, voltage_domain=system.voltage_domain
)

init_progress("creating Ruby controllers and network topology")
Ruby.create_system(args, False, system)
init_progress("Ruby controllers and topology created; connecting traffic endpoints")

system.ruby.clk_domain = SrcClockDomain(
    clock=args.ruby_clock, voltage_domain=system.voltage_domain
)

i = 0
for ruby_port in system.ruby._cpu_ports:
    cpus[i].test = ruby_port.in_ports
    i += 1


root = Root(full_system=False, system=system)
root.system.mem_mode = "timing"

m5.ticks.setGlobalFrequency("1ps")

init_progress("m5.instantiate: creating C++ objects, connecting ports and initializing network")
m5._omelet_init_progress = args.online_control
if args.online_control:
    os.environ["OMELET_INIT_PROGRESS"] = "1"
if args.online_control and os.environ.get("OMELET_STATS_TRACE") == "1":
    original_visit_stats = m5.stats._visit_stats

    def trace_visit_stats(visitor, root=None):
        visited = 0

        def visit(group, stat):
            nonlocal visited
            dimension = stat.size if hasattr(stat, "size") else None
            if stat.name in (
                "system.mem_ctrls0000.dram.bytesRead",
                "system.mem_ctrls0000.requestorReadAccesses",
            ):
                init_progress(f"requestor statistic name={stat.name} size={dimension}")
            if dimension is not None and dimension > 100000:
                init_progress(f"large statistic name={stat.name} size={dimension}")
            try:
                visitor(group, stat)
            except MemoryError:
                init_progress(
                    f"statistic allocation failed name={stat.name} "
                    f"type={type(stat).__name__} size={dimension} "
                    f"visitor={visitor.__name__} visited={visited}"
                )
                raise
            visited += 1
            if visited % 65536 == 0:
                init_progress(f"statistics visitor={visitor.__name__} visited={visited}")

        return original_visit_stats(visit, root=root)

    m5.stats._visit_stats = trace_visit_stats

if args.online_control:
    original_enable_stats = m5.stats.enable

    def enable_stats_with_progress():
        init_progress("enabling statistics started")
        result = original_enable_stats()
        init_progress("enabling statistics completed")
        return result

    m5.stats.enable = enable_stats_with_progress

if args.online_control and os.environ.get("OMELET_COMM_DUMP_STATS") == "0":
    # The online controller does not consume gem5's object dump. Keep the
    # SimObjects and instantiate passes, but avoid serializing a large debug
    # file for the normal non-debug communication path.
    m5.options.dump_config = ""
    m5.options.json_config = ""
    m5.options.dot_config = ""
    m5.options.dot_dvfs_config = ""
m5.instantiate()
init_progress("m5.instantiate completed; network ready")

if args.online_control:
    print("OMELET_ONLINE " + json.dumps({"ready": True, "tick": m5.curTick()}), flush=True)
    for line in sys.stdin:
        command = json.loads(line)
        op = command["op"]
        if op == "inject":
            src = int(command["src"])
            if src < 0 or src >= len(cpus):
                raise ValueError(f"invalid source controller: {src}")
            cpus[src].sendOnlinePacket(
                int(command["dst"]), int(command["bytes"]), int(command["id"])
            )
            result = {"tick": m5.curTick()}
        elif op == "advance":
            ticks = int(command["ticks"])
            if ticks <= 0:
                raise ValueError("advance ticks must be positive")
            event = m5.simulate(ticks)
            arrivals = []
            for row in cpus[0].drainOnlineArrivals().splitlines():
                packet_id, tick = map(int, row.split())
                arrivals.append({"id": packet_id, "tick": tick})
            result = {"tick": m5.curTick(), "cause": event.getCause(), "arrivals": arrivals}
        elif op == "state":
            result = {"tick": m5.curTick(), "in_flight": cpus[0].onlineInFlight()}
        elif op == "run_cpp":
            result_path = cpus[0].runOnlineWorkload(
                str(command["manifest"]), str(command["position"]),
                str(command["catalog"]), str(command["nonuniform_grid"]),
                str(command["endpoint_mode"]), int(command["k_node"]),
                int(command["k_chip"]), float(command["frequency_ghz"]),
                str(command["result_path"]),
            )
            result = {
                "tick": m5.curTick(), "done": True,
                "result_path": result_path,
                "in_flight": cpus[0].onlineInFlight(),
            }
            with open(result_path, encoding="utf-8") as result_file:
                summary = json.load(result_file)
            result.update({
                "injected": int(summary["message_injected"]),
                "arrived": int(summary["message_arrived"]),
            })
        elif op == "quit":
            if cpus[0].onlineInFlight() != 0:
                raise RuntimeError("cannot quit with online packets in flight")
            result = {"tick": m5.curTick(), "done": True}
        else:
            raise ValueError(f"unknown online operation: {op}")
        print("OMELET_ONLINE " + json.dumps(result), flush=True)
        if op == "quit":
            if os.environ.get("OMELET_COMM_DUMP_STATS") == "0":
                # Only the optional dump is removed; gem5 exit cleanup remains.
                import atexit
                atexit.unregister(m5.stats.dump)
            break
else:
    exit_event = m5.simulate(args.abs_max_tick)
    print("Exiting @ tick", m5.curTick(), "because", exit_event.getCause())
