# PowerPoint conversion fixtures

All course text, values, diagrams and images in these files are original synthetic QA content. No user's course or account data is included.

- `circuits.pptx`: two slides, editable French text, Unicode Ω/μ/τ, a first-order RC formula written as text, and an embedded original graph. Created with `@oai/artifact-tool`, then structurally validated and visually inspected.
- `circuits.ppt`: real OLE compound-file PowerPoint 97 format exported from the first fixture by LibreOffice Impress 25.2.3.2 ARM64. It is not a renamed PPTX.
- `circuits-equation-no-preview.pptx`: the first slide's formula uses native Office Math (OMML), including subscript, exponent and fraction, inside an Office 2010 alternate-content shape. The file was opened and serialized using installed Microsoft PowerPoint; its reference PDF displays the actual equation. Its fallback deliberately contains a visible sentinel sentence instead of an image. LibreOffice displays that fallback sentence. The backend must refuse this precise case, never claim the formula survived.
- `circuits-equation.pptx`: the same native equation, with a compatible PNG exported directly by Microsoft PowerPoint from the equation shape and embedded as the alternate-content image. The ARM runtime preserves the displayed equation through this image, plus the second slide's graph and surrounding editable text.

The equation preview is part of the input fixture. Production conversion does not invoke Microsoft PowerPoint or create replacement images. Native Office was used locally only to validate the reference and create its compatible preview.

The process-boundary tests use a ZIP comment to tell a dedicated fake converter which failure mode to exercise. Real-engine tests use these unchanged files and are enabled through `REPAPER_OFFICE_REAL_CONVERTER`. Header assertions alone do not establish visual fidelity; the corresponding PDFs must also be rendered and compared with the fixture/reference.
