/* zportal pick open|save|folder [--multiple] [--title T] [--accept LABEL]
 *              [--filters EXTS] [--folder DIR] [--name NAME]
 *
 * The file chooser a Linux app gets when it asks the FileChooser portal:
 * Zurbax's own, in a window of its own. Prints the chosen path and exits 0, or
 * exits 1 when the user cancels. */
#include "zportal.h"

static struct {
	u32 mode;
	char title[256], filters[128], folder[512], name[256];
	bool started;
} pk;

static void pick_frame(Ui *ui)
{
	if (!pk.started) {
		pk.started = true;
		ui_file_dialog_open(ui, "pick", pk.mode, pk.folder, pk.filters, pk.name[0] ? pk.name : NULL);
	}
	ui_fill(ui, rect(0, 0, ui->width, ui->height), ui->theme.window);
	char out[1024];
	if (ui_file_dialog(ui, "pick", out, sizeof out)) {
		print("%s\n", out);
		exit(0);
	}
	if (!ui_file_dialog_is_open(ui, "pick"))
		exit(1);
}

int picker_main(int argc, char **argv)
{
	pk.mode = FILE_OPEN;
	if (argc > 2)
		pk.mode = str_eq(argv[2], "save") ? FILE_SAVE : str_eq(argv[2], "folder") ? FILE_FOLDER : FILE_OPEN;
	for (int i = 3; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
		if (str_eq(a, "--title"))
			str_copy(pk.title, v, sizeof pk.title), i++;
		else if (str_eq(a, "--filters"))
			str_copy(pk.filters, v, sizeof pk.filters), i++;
		else if (str_eq(a, "--folder"))
			str_copy(pk.folder, v, sizeof pk.folder), i++;
		else if (str_eq(a, "--name"))
			str_copy(pk.name, v, sizeof pk.name), i++;
		else if (str_eq(a, "--accept"))
			i++;
	}
	if (!pk.title[0])
		str_copy(pk.title, pk.mode == FILE_SAVE ? "Save File" : pk.mode == FILE_FOLDER ? "Choose a Folder" : "Open File",
			 sizeof pk.title);
	UiAppDesc desc = { .id = "vault", .title = pk.title, .width = 800, .height = 560, .min_width = 520, .min_height = 400,
			   .flags = WIN_RESIZABLE | WIN_KEEP_ABOVE, .frame = pick_frame };
	ui_run(&desc);
	return 1;
}
