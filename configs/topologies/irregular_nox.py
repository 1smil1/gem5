"""Nonuniform chiplet topology using the shared PPO layout description."""

from .mesh_nox import mesh_nox


class irregular_nox(mesh_nox):
    description = "irregular_nox"
