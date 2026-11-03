-- The interpreted ages of public releases in the terms of the USGS Geochron
-- database (data publishing and hosted sharing design,
-- docs/superpowers/specs/2026-10-18-data-publishing-and-hosted-sharing-design.md,
-- section 9.2; USGS Data Report 1227, table 2.1): an OGC API collection a
-- reader can load beside USGS's ar40ar39 table. Additive only: a build before
-- this migration keeps opening a migrated store.
--
-- One row per interpreted age, as USGS's parent row: column names and units
-- are theirs, flattened with sample and location as their data release is.
-- Steps are not here: their values are in pub_analysis_value, which only
-- `publish upload` will write. Values come straight from the ia_value row the
-- release pinned; nothing is calculated but a change of unit.
--
-- ia_value.age_err is one sigma, as USGS's ageunc. It says nothing reliable
-- about J (include_j_error_in_plateau is set on documents whose error has
-- none), so ageuncint and ageuncext are left out.
--
-- The name decayconttotalunc is USGS's spelling. A sample with no location is
-- left out: USGS requires one. Same rule as the release build for members
-- (spec R4): an age is served only when every member is in the release, not
-- under embargo, and of a visible sample.
-- @sqlite skip
CREATE VIEW pub_geochron_ar40ar39 AS
  SELECT r.uuid::text || '_' || ia.uuid::text AS id,
         s.name AS sample_id, s.igsn, s.note AS samplecomments, s.elevation,
         public.ST_Y(s.geom) AS lat, public.ST_X(s.geom) AS long, 'WGS84' AS publication_geodatum,
         s.lithology AS fieldname, s.unit AS geologicalunit, s.location AS localitydesc,
         lower(m.name) AS samplematerial,
         first_member.identifier AS argon_id, first_member.laboratory AS lab,
         v.age, v.age_err AS ageunc, v.mswd, v.nanalyses AS nofanalyses,
         v.kca AS kcaratio, v.kca_err AS kcaratiounc,
         kind.ageintpntype, 'not specified' AS ageintpnclass, kind.analysistype,
         'Noble gas mass spectrometer' AS instrumentmethod,
         CASE WHEN jsonb_typeof(c.constants->'lambda_k') = 'number'
              THEN (c.constants->'lambda_k')::double precision * 1e10 END AS decayconsttotal,
         CASE WHEN jsonb_typeof(c.constants->'lambda_k_err') = 'number'
              THEN (c.constants->'lambda_k_err')::double precision * 1e10 END AS decayconttotalunc,
         -- assumed for a plateau or a mean; an isochron's is calculated, and is not in ia_value
         CASE WHEN kind.ageintpntype <> 'isochron' AND jsonb_typeof(c.constants->'atm4036') = 'number'
              THEN (c.constants->'atm4036')::double precision END AS initratioar4036,
         CASE WHEN kind.ageintpntype <> 'isochron' AND jsonb_typeof(c.constants->'atm4036_err') = 'number'
              THEN (c.constants->'atm4036_err')::double precision END AS initratioar4036unc,
         r.title AS citation,
         CASE WHEN r.doi IS NULL THEN NULL ELSE 'https://doi.org/' || r.doi END AS url,
         extract(year FROM r.published_utc)::int AS publication_year,
         'Digital Repository' AS datasrctype,
         r.published_utc AS entrydate, s.updated_utc AS lastupdatestamp,
         ia.name AS ia_name, ia.uuid AS ia_uuid, s.uuid AS sample_uuid, ria.revision_uuid,
         r.name AS release_name, r.version AS release_version, s.geom
  FROM release_interpreted_age ria
  JOIN pub_visible_release r ON r.uuid = ria.release_uuid
  JOIN interpreted_age ia ON ia.uuid = ria.ia_uuid
  JOIN ia_value v ON v.revision_uuid = ria.revision_uuid
  -- the newest visible version of each release name that holds the age, aggregated once
  JOIN (SELECT r2.name, ria2.ia_uuid, max(r2.version) AS version FROM release_interpreted_age ria2
        JOIN pub_visible_release r2 ON r2.uuid = ria2.release_uuid GROUP BY r2.name, ria2.ia_uuid) nv
    ON nv.name = r.name AND nv.version = r.version AND nv.ia_uuid = ria.ia_uuid
  -- the sample and the identifier: those of the earliest member
  JOIN LATERAL (SELECT pas.sample_uuid, i.identifier, a.laboratory FROM ia_member im
                JOIN analysis a ON a.uuid = im.analysis_uuid
                JOIN identifier i ON i.uuid = a.identifier_uuid
                JOIN pub_analysis_sample pas ON pas.analysis_uuid = a.uuid
                WHERE im.revision_uuid = ria.revision_uuid
                ORDER BY a.timestamp_utc, a.uuid LIMIT 1) first_member ON true
  JOIN pub_visible_sample s ON s.uuid = first_member.sample_uuid
  JOIN material m ON m.uuid = s.material_uuid
  -- a nested legacy document keeps them under "preferred"
  CROSS JOIN LATERAL (SELECT coalesce(v.doc#>'{preferred,arar_constants}', v.doc->'arar_constants') AS constants) c
  -- USGS's vocabulary (their ar40ar39 table, version 4.0)
  CROSS JOIN LATERAL (SELECT CASE WHEN v.age_kind ILIKE '%plateau%' THEN 'plateau'
                                  WHEN v.age_kind ILIKE '%weighted%mean%' THEN 'weighted mean'
                                  WHEN v.age_kind ILIKE '%isochron%' THEN 'isochron'
                                  WHEN v.age_kind ILIKE '%integrated%' THEN 'total gas'
                                  ELSE 'not specified' END AS ageintpntype,
                             CASE WHEN v.age_kind ILIKE '%plateau%' OR v.age_kind ILIKE '%integrated%'
                                  THEN 'Incremental heating' ELSE 'not specified' END AS analysistype) kind
  WHERE s.geom IS NOT NULL
    AND NOT EXISTS (SELECT 1 FROM ia_member im
                    WHERE im.revision_uuid = ria.revision_uuid
                      AND NOT EXISTS (SELECT 1 FROM analysis_group_member gm
                                      WHERE gm.group_uuid = r.group_uuid AND gm.analysis_uuid = im.analysis_uuid))
    AND NOT EXISTS (SELECT 1 FROM ia_member im
                    JOIN pub_live_embargo e ON e.scope_type = 'analysis' AND e.scope_uuid = im.analysis_uuid
                    WHERE im.revision_uuid = ria.revision_uuid)
    AND NOT EXISTS (SELECT 1 FROM ia_member im
                    JOIN pub_analysis_sample pas ON pas.analysis_uuid = im.analysis_uuid
                    LEFT JOIN pub_visible_sample vs ON vs.uuid = pas.sample_uuid
                    WHERE im.revision_uuid = ria.revision_uuid AND vs.uuid IS NULL);
