"""Offline Draft 2020-12 validation for Sitometron development tooling."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterator
from urllib.parse import urljoin, urlparse

from jsonschema import Draft202012Validator
from referencing import Registry, Resource
from referencing.exceptions import NoSuchResource, Unresolvable
from referencing.jsonschema import DRAFT202012

_DRAFT_2020_12 = "https://json-schema.org/draft/2020-12/schema"


def _deny_external_retrieval(uri: str) -> Resource[Any]:
    raise NoSuchResource(ref=uri)


def _require_local_uri(uri: str) -> None:
    parsed = urlparse(uri)
    if parsed.scheme or parsed.netloc or "\\" in uri:
        raise Unresolvable(ref=uri)


def _schema_resources(
    resource: Resource[Any], base_uri: str
) -> Iterator[tuple[Resource[Any], str]]:
    resource_id = resource.id()
    current_base = urljoin(base_uri, resource_id) if resource_id is not None else base_uri
    _require_local_uri(current_base)
    yield resource, current_base
    for subresource in resource.subresources():
        yield from _schema_resources(subresource, current_base)


@dataclass(frozen=True)
class LocalDraft202012Schemas:
    """A checked local-only schema registry rooted at one directory."""

    root: Path
    schemas: dict[Path, dict[str, Any]]
    retrieval_uris: dict[Path, str]
    registry: Registry[Any]

    @classmethod
    def load(cls, root: Path) -> "LocalDraft202012Schemas":
        resolved_root = root.resolve(strict=True)
        schema_paths = sorted(resolved_root.rglob("*.schema.json"))
        if not schema_paths:
            raise ValueError(f"no *.schema.json files under {resolved_root}")

        documents: dict[Path, dict[str, Any]] = {}
        retrieval_uris: dict[Path, str] = {}
        registry: Registry[Any] = Registry(retrieve=_deny_external_retrieval)

        for path in schema_paths:
            document = json.loads(path.read_text(encoding="utf-8"))
            if not isinstance(document, dict):
                raise TypeError(f"schema document must be an object: {path}")
            if document.get("$schema") != _DRAFT_2020_12:
                raise ValueError(f"schema must declare Draft 2020-12: {path}")
            Draft202012Validator.check_schema(document)
            resolved_path = path.resolve()
            documents[resolved_path] = document

            resource = Resource.from_contents(document, default_specification=DRAFT202012)
            relative_uri = path.relative_to(resolved_root).as_posix()
            retrieval_uris[resolved_path] = relative_uri
            schema_id = document.get("$id", relative_uri)
            if not isinstance(schema_id, str) or not schema_id:
                raise ValueError(f"schema $id must be a non-empty string: {path}")
            _require_local_uri(schema_id)

            registry = registry.with_resource(relative_uri, resource)

        registry = registry.crawl()
        loaded = cls(
            root=resolved_root,
            schemas=documents,
            retrieval_uris=retrieval_uris,
            registry=registry,
        )
        loaded._validate_all_references()
        return loaded

    def _validate_all_references(self) -> None:
        for path, document in self.schemas.items():
            base_uri = path.relative_to(self.root).as_posix()
            resource = Resource.from_contents(document, default_specification=DRAFT202012)
            for subresource, effective_base in _schema_resources(resource, base_uri):
                contents = subresource.contents
                if not isinstance(contents, dict):
                    continue
                resolver = self.registry.resolver(base_uri=effective_base)
                for keyword in ("$ref", "$dynamicRef"):
                    reference = contents.get(keyword)
                    if isinstance(reference, str):
                        _require_local_uri(reference)
                        resolver.lookup(reference)

    def validate_instance(self, schema_path: Path, instance: object) -> None:
        resolved_path = schema_path.resolve(strict=True)
        try:
            schema = self.schemas[resolved_path]
        except KeyError as error:
            raise ValueError(f"schema is outside the loaded registry: {resolved_path}") from error
        retrieval_uri = self.retrieval_uris[resolved_path]
        schema_id = schema.get("$id")
        effective_id = urljoin(retrieval_uri, schema_id) if schema_id is not None else retrieval_uri
        schema_for_validation = dict(schema)
        schema_for_validation["$id"] = effective_id
        Draft202012Validator(
            schema_for_validation,
            registry=self.registry,
            format_checker=Draft202012Validator.FORMAT_CHECKER,
        ).validate(instance)
