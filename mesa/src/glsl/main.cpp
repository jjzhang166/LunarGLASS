/*
 * Copyright © 2008, 2009 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include <stdlib.h>
#include <stdio.h>

//LunarGLASS uses its own option parsing
//#include <getopt.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
typedef int ssize_t;
#else
#include <unistd.h>
#endif

#include "ast.h"
#include "glsl_parser_extras.h"
#include "glsl_parser.h"
#include "ir_optimization.h"
#include "ir_print_visitor.h"
#include "program.h"
#include "loop_analysis.h"

#include "ir_to_mesa.h"

// Begin: LunarG
// STL and C++ utils, used for argument handling
#include <vector>
#include <string>
#include <iostream>

// LunarGLASS runtime options handling
#include "OptionParse.h"
#include "Options.h"

// LunarGLASS includes
#include "LunarGLASSManager.h"
#include "Frontends/Glsl2/GlslToTop.h"

// End: LunarG
extern "C" struct gl_shader *
_mesa_new_shader(struct gl_context *ctx, GLuint name, GLenum type);

extern "C" void
_mesa_reference_shader(struct gl_context *ctx, struct gl_shader **ptr,
                       struct gl_shader *sh);

/* Copied from shader_api.c for the stand-alone compiler.
 */
void
_mesa_reference_shader(struct gl_context *ctx, struct gl_shader **ptr,
                       struct gl_shader *sh)
{
   *ptr = sh;
}

struct gl_shader *
_mesa_new_shader(struct gl_context *ctx, GLuint name, GLenum type)
{
   struct gl_shader *shader;

   (void) ctx;

   assert(type == GL_FRAGMENT_SHADER || type == GL_VERTEX_SHADER);
   shader = rzalloc(NULL, struct gl_shader);
   if (shader) {
      shader->Type = type;
      shader->Name = name;
      shader->RefCount = 1;
   }
   return shader;
}

static void
initialize_context(struct gl_context *ctx, gl_api api)
{
   memset(ctx, 0, sizeof(*ctx));

   ctx->API = api;

   ctx->Extensions.ARB_ES2_compatibility = GL_TRUE;
   ctx->Extensions.ARB_draw_buffers = GL_TRUE;
   ctx->Extensions.ARB_fragment_coord_conventions = GL_TRUE;
   ctx->Extensions.EXT_texture_array = GL_TRUE;
   ctx->Extensions.NV_texture_rectangle = GL_TRUE;

   /* GLSL 1.30 isn't fully supported, but we need to advertise 1.30 so that
    * the built-in functions for 1.30 can be built.
    */
   ctx->Const.GLSLVersion = 130;

   /* 1.10 minimums. */
   ctx->Const.MaxLights = 8;
   ctx->Const.MaxClipPlanes = 8;
   ctx->Const.MaxTextureUnits = 2;

   /* More than the 1.10 minimum to appease parser tests taken from
    * apps that (hopefully) already checked the number of coords.
    */
   ctx->Const.MaxTextureCoordUnits = 4;

   ctx->Const.VertexProgram.MaxAttribs = 16;
   ctx->Const.VertexProgram.MaxUniformComponents = 512;
   ctx->Const.MaxVarying = 8;
   ctx->Const.MaxVertexTextureImageUnits = 0;
   ctx->Const.MaxCombinedTextureImageUnits = 2;
   ctx->Const.MaxTextureImageUnits = 2;
   ctx->Const.FragmentProgram.MaxUniformComponents = 64;

   ctx->Const.MaxDrawBuffers = 2;

   ctx->Driver.NewShader = _mesa_new_shader;
}

/* Returned string will have 'ctx' as its ralloc owner. */
static char *
load_text_file(void *ctx, const char *file_name)
{
	char *text = NULL;
	struct stat st;
	ssize_t total_read = 0;
	int fd = open(file_name, O_RDONLY);

	if (fd < 0) {
		return NULL;
	}

	if (fstat(fd, & st) == 0) {
	   text = (char *) ralloc_size(ctx, st.st_size + 1);
		if (text != NULL) {
			do {
				ssize_t bytes = read(fd, text + total_read,
						     st.st_size - total_read);
				if (bytes < 0) {
					free(text);
					text = NULL;
					break;
				}

				if (bytes == 0) {
					break;
				}

				total_read += bytes;
			} while (total_read < st.st_size);

			text[total_read] = '\0';
		}
	}

	close(fd);

	return text;
}

int glsl_es = 0;
int dump_ast = 0;
int dump_hir = 0;
int dump_lir = 0;
int do_link = 0;
int do_glsl_to_mesa_ir = 0;
bool do_cross_stage;

const struct { const char* arg; int foo; int* flag; int bar;} compiler_opts[] = {
   { "glsl-es",  0, &glsl_es,  1 },
   { "dump-ast", 0, &dump_ast, 1 },
   { "dump-hir", 0, &dump_hir, 1 },
   { "dump-lir", 0, &dump_lir, 1 },
   { "link",     0, &do_link,  1 },
   { NULL, 0, NULL, 0 }
};

void
usage_fail(const char *name)
{
      printf("%s <filename.frag|filename.vert>\n", name);
      exit(EXIT_FAILURE);
}


void
compile_shader(struct gl_context *ctx, struct gl_shader *shader)
{
   struct _mesa_glsl_parse_state *state =
      new(shader) _mesa_glsl_parse_state(ctx, shader->Type, shader);

   const char *source = shader->Source;
   state->error = preprocess(state, &source, &state->info_log,
			     state->extensions, ctx->API);

   if (!state->error) {
      _mesa_glsl_lexer_ctor(state, source);
      _mesa_glsl_parse(state);
      _mesa_glsl_lexer_dtor(state);
   }

   if (dump_ast) {
      foreach_list_const(n, &state->translation_unit) {
	 ast_node *ast = exec_node_data(ast_node, n, link);
	 ast->print();
     printf("\n");
      }
      printf("\n\n");
   }

   shader->ir = new(shader) exec_list;
   if (!state->error && !state->translation_unit.is_empty())
      _mesa_ast_to_hir(shader->ir, state);

   /* Print out the unoptimized IR. */
   if (!state->error && dump_hir) {
      validate_ir_tree(shader->ir);
      _mesa_print_ir(shader->ir, state);
   }

   /* Optimization passes */
   if (!state->error && !shader->ir->is_empty()) {
      bool progress;
      do {

     //progress = false;
     progress = do_common_optimization(shader->ir, false, 0);

      } while (progress);

      validate_ir_tree(shader->ir);
   }


   /* Print out the resulting IR */
   if (!state->error && dump_lir) {
      _mesa_print_ir(shader->ir, state);
   }

   shader->symbols = state->symbols;
   shader->CompileStatus = !state->error;
   shader->Version = state->language_version;
   memcpy(shader->builtins_to_link, state->builtins_to_link,
	  sizeof(shader->builtins_to_link[0]) * state->num_builtins_to_link);
   shader->num_builtins_to_link = state->num_builtins_to_link;

   if (shader->InfoLog)
      ralloc_free(shader->InfoLog);

   shader->InfoLog = state->info_log;

   /* Retain any live IR, but trash the rest. */
   reparent_ir(shader->ir, shader);

   ralloc_free(state);

   return;
}

int
main(int argc, char **argv)
{
   int status = EXIT_SUCCESS;
   struct gl_context local_ctx;
   struct gl_context *ctx = &local_ctx;

   int idx = 0;

   if (argc < 2)
      gla::PrintHelp();

   dump_hir = 0;
   dump_lir = 0;
   do_link = 1;

   // LunarGLASS cannot yet translate the linked version of a stage.
   // But, we need to make progress doing so.  This is the flag to
   // set to switch from unlinked to linked.
   bool translate_linked_shader = true;

   // Handle some LunarGLASS specific options in a more platform-independent manner
   // Overwrites argc and argv
   int optind = gla::HandleArgs(argc, argv);
   dump_ast = gla::Options.debug;
   do_glsl_to_mesa_ir = gla::Options.backend == gla::TGSI;
   if (do_glsl_to_mesa_ir)
       do_cross_stage = true;

   // LunarGOO will sometimes want to translate a single stage
   // without seeing other stages, but still not "optimize"
   // as if that stage is truly missing in the pipeline.
   do_cross_stage = gla::Options.optimizations.crossStage;

   initialize_context(ctx, (glsl_es) ? API_OPENGLES2 : API_OPENGL);

   struct gl_shader_program *whole_program;

   whole_program = rzalloc (NULL, struct gl_shader_program);
   assert(whole_program != NULL);

   for (/* empty */; argc > optind; optind++) {
       if (gla::Options.debug && ! gla::Options.bottomIROnly)
         printf("compiling %s...\n", argv[optind]);

      whole_program->Shaders = (struct gl_shader **)
	 reralloc(whole_program, whole_program->Shaders,
			struct gl_shader *, whole_program->NumShaders + 1);
      assert(whole_program->Shaders != NULL);

      struct gl_shader *shader = rzalloc(whole_program, gl_shader);

      whole_program->Shaders[whole_program->NumShaders] = shader;
      whole_program->NumShaders++;

      const unsigned len = strlen(argv[optind]);
      if (len < 6)
	 usage_fail(argv[0]);

      const char *const ext = & argv[optind][len - 5];
      if (strncmp(".vert", ext, 5) == 0)
	 shader->Type = GL_VERTEX_SHADER;
      else if (strncmp(".geom", ext, 5) == 0)
	 shader->Type = GL_GEOMETRY_SHADER;
      else if (strncmp(".frag", ext, 5) == 0)
	 shader->Type = GL_FRAGMENT_SHADER;
      else
	 usage_fail(argv[0]);

      shader->Source = load_text_file(whole_program, argv[optind]);
      if (shader->Source == NULL) {
	 printf("File \"%s\" does not exist.\n", argv[optind]);
	 exit(EXIT_FAILURE);
      }

      compile_shader(ctx, shader);

      if (!shader->CompileStatus) {
	 printf("Info log for %s:\n%s\n", argv[optind], shader->InfoLog);
	 status = EXIT_FAILURE;
	 break;
      }
   }

   if ((status == EXIT_SUCCESS) && do_link)  {
      link_shaders(ctx, whole_program);
      status = (whole_program->LinkStatus) ? EXIT_SUCCESS : EXIT_FAILURE;

      if (strlen(whole_program->InfoLog) > 0)
	 printf("Info log for linking:\n%s\n", whole_program->InfoLog);
   }

   // Insert the LunarGLASS path into this process...
   if (status == EXIT_SUCCESS) {

      assert(whole_program->NumShaders == 1);
      struct gl_shader *Shader;
      if (translate_linked_shader) {
          // assuming just one stage for now...  find it
          for (int i = 0; i < MESA_SHADER_TYPES; ++i) {
              Shader = whole_program->_LinkedShaders[i];
              if (Shader)
                  break;
          }
      }
      else
         Shader = whole_program->Shaders[0];

      if (gla::Options.debug && ! gla::Options.bottomIROnly)
         _mesa_print_ir(Shader->ir, 0);
      
      gla::Manager* glaManager = gla::getManager();
      int compileCount = gla::Options.iterate ? 1000 : 1;
      for (int i = 0; i < compileCount; ++i) {
          TranslateGlslToTop(Shader, glaManager);

          glaManager->translateTopToBottom();

          glaManager->translateBottomToTarget();

          glaManager->clear();
      }

      delete glaManager;
   }


   if(status == EXIT_SUCCESS && do_glsl_to_mesa_ir) {
       ctx->Shader.Flags |= GLSL_DUMP;
       // Turn on this flag to disable Mesa IR optimizations
       //ctx->Shader.Flags |= GLSL_NO_OPT;
       _mesa_ir_link_shader(ctx, whole_program);
   }

   for (unsigned i = 0; i < MESA_SHADER_TYPES; i++)
      ralloc_free(whole_program->_LinkedShaders[i]);

   ralloc_free(whole_program);
   _mesa_glsl_release_types();
   _mesa_glsl_release_functions();

   return status;
}
