package corefixture;

import java.util.Date;
import java.util.TimeZone;
import javax.microedition.lcdui.Alert;
import javax.microedition.lcdui.AlertType;
import javax.microedition.lcdui.Choice;
import javax.microedition.lcdui.ChoiceGroup;
import javax.microedition.lcdui.Command;
import javax.microedition.lcdui.CommandListener;
import javax.microedition.lcdui.CustomItem;
import javax.microedition.lcdui.DateField;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Displayable;
import javax.microedition.lcdui.Form;
import javax.microedition.lcdui.Font;
import javax.microedition.lcdui.Gauge;
import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.Image;
import javax.microedition.lcdui.ImageItem;
import javax.microedition.lcdui.Item;
import javax.microedition.lcdui.ItemCommandListener;
import javax.microedition.lcdui.ItemStateListener;
import javax.microedition.lcdui.Spacer;
import javax.microedition.lcdui.StringItem;
import javax.microedition.lcdui.TextBox;
import javax.microedition.lcdui.TextField;
import javax.microedition.lcdui.Ticker;
import javax.microedition.midlet.MIDlet;

public final class LcduiExtendedOps extends MIDlet
        implements ItemStateListener, ItemCommandListener, CommandListener {
    private static LcduiExtendedOps app;
    private static Display display;
    private static Form form;
    private static Ticker ticker;
    private static TextField textField;
    private static TextField numericField;
    private static StringItem styledItem;
    private static Gauge gauge;
    private static Gauge indicator;
    private static ChoiceGroup choice;
    private static DateField dateField;
    private static DateField dateOnly;
    private static DateField timeOnly;
    private static ImageItem imageItem;
    private static Spacer spacer;
    private static Probe customItem;
    private static TextBox textBox;
    private static TextBox constrainedBox;
    private static Alert alert;
    private static Alert foreverAlert;
    private static Command staleCommand;
    private static StringItem orderProbe;

    private static int stateCalls;
    private static int itemCommandCalls;
    private static int screenCommandCalls;
    private static int listenerMutations;
    private static int ephemeralIndex;
    private static boolean foreverAccepted;
    private static boolean textFieldCharOpsPassed;
    private static boolean displayCapabilitiesPassed;
    private static boolean choiceFontPassed;
    private static boolean formArrayPassed;
    private static Font customFont;

    protected void startApp() {}
    protected void pauseApp() {}
    protected void destroyApp(boolean unconditional) {}

    public static int setup() {
        app = new LcduiExtendedOps();
        display = Display.getDisplay(app);
        form = new Form("Extended");
        ticker = new Ticker("ticker-one");
        form.setTicker(ticker);
        form.setItemStateListener(app);

        textField = new TextField("Text", "old", 32, TextField.ANY);
        numericField = new TextField("Numeric", "12", 8,
                TextField.NUMERIC | TextField.PASSWORD);
        customFont = Font.getFont(Font.FACE_SYSTEM, Font.STYLE_BOLD,
                                  Font.SIZE_SMALL);
        styledItem = new StringItem("Styled", "font");
        styledItem.setFont(customFont);
        gauge = new Gauge("Gauge", true, 10, 1);
        Image image = Image.createImage(2, 3);
        choice = new ChoiceGroup("Choice", Choice.MULTIPLE,
                                 new String[] {"zero", "one"},
                                 new Image[] {image, null});
        choice.setFont(1, customFont);
        choice.insert(0, "inserted", null);
        choiceFontPassed = choice.getFont(2) == customFont;
        choice.delete(0);
        choiceFontPassed = choiceFontPassed && choice.getFont(1) == customFont;
        dateField = new DateField("DateTime", DateField.DATE_TIME,
                                  TimeZone.getTimeZone("GMT+07:00"));
        dateOnly = new DateField("Date", DateField.DATE);
        timeOnly = new DateField("Time", DateField.TIME);
        imageItem = new ImageItem("Image", image, 0, "image-alt", 2);
        spacer = new Spacer(4, 5);
        customItem = new Probe("Custom");

        StringItem arrayFirst = new StringItem("ArrayA", "first");
        StringItem arraySecond = new StringItem("ArrayB", "second");
        Form arrayForm = new Form("ArrayForm",
                new Item[] {arrayFirst, arraySecond});
        boolean nullItemRejected = false;
        try {
            new Form("BadArrayForm", new Item[] {null});
        } catch (NullPointerException expected) {
            nullItemRejected = true;
        }
        formArrayPassed = arrayForm.size() == 2 &&
                arrayForm.get(0) == arrayFirst &&
                arrayForm.get(1) == arraySecond && nullItemRejected;

        Command itemCommand = new Command("Use", Command.ITEM, 0);
        customItem.addCommand(itemCommand);
        customItem.setDefaultCommand(itemCommand);
        customItem.setItemCommandListener(app);

        form.append(textField);
        form.append(numericField);
        form.append(styledItem);
        form.append(gauge);
        form.append(choice);
        form.append(dateField);
        form.append(dateOnly);
        form.append(timeOnly);
        form.append(imageItem);
        form.append(spacer);
        form.append(customItem);
        form.append(image);
        ephemeralIndex = form.append(new StringItem("Ephemeral", "stale"));

        form.addCommand(new Command("Help", Command.HELP, 2));
        form.addCommand(new Command("Back", Command.BACK, 5));
        form.addCommand(new Command("OK", Command.OK, 7));
        staleCommand = new Command("Stale", Command.SCREEN, 99);
        form.addCommand(staleCommand);
        form.setCommandListener(app);
        display.setCurrent(form);
        display.setCurrentItem(styledItem);
        boolean badColorRejected = false;
        try {
            display.getColor(99);
        } catch (IllegalArgumentException expected) {
            badColorRejected = true;
        }
        boolean badFontRejected = false;
        try {
            Font.getFont(99);
        } catch (IllegalArgumentException expected) {
            badFontRejected = true;
        }
        displayCapabilitiesPassed = display.getCurrent() == form &&
                display.getColor(Display.COLOR_BACKGROUND) >= 0 &&
                display.getColor(Display.COLOR_FOREGROUND) >= 0 &&
                display.getBorderStyle(false) == Graphics.SOLID &&
                Font.getFont(Font.FONT_STATIC_TEXT) != null &&
                Font.getFont(Font.FONT_INPUT_TEXT) != null &&
                badColorRejected && badFontRejected;

        char[] fieldChars = new char[] {'a', 'b', 'c'};
        textField.setChars(fieldChars, 0, fieldChars.length);
        textField.insert("X", 1);
        char[] fieldInsert = new char[] {'Y', 'Z'};
        textField.insert(fieldInsert, 1, 1, 4);
        textField.delete(1, 2);
        char[] fieldCopy = new char[4];
        textFieldCharOpsPassed = "acZ".equals(textField.getString()) &&
                textField.getChars(fieldCopy) == 3 &&
                fieldCopy[0] == 'a' && fieldCopy[1] == 'c' &&
                fieldCopy[2] == 'Z' && textField.getCaretPosition() == 1;

        textBox = new TextBox("Box", "start", 32, 0);
        char[] first = new char[] {'a', 'b', 'c'};
        textBox.setChars(first, 0, first.length);
        textBox.insert("X", 1);
        char[] second = new char[] {'Y', 'Z'};
        textBox.insert(second, 1, 1, 4);
        textBox.delete(1, 2);
        constrainedBox = new TextBox("PIN", "12", 8,
                TextField.NUMERIC | TextField.PASSWORD);
        boolean rejected = false;
        try {
            constrainedBox.setString("1x");
        } catch (IllegalArgumentException expected) {
            rejected = true;
        }
        if (!rejected) return 91;
        constrainedBox.setString("-42");

        Alert commandProbe = new Alert("Commands");
        commandProbe.setTimeout(333);
        Command firstAlertCommand = new Command("One", Command.OK, 1);
        Command secondAlertCommand = new Command("Two", Command.CANCEL, 2);
        commandProbe.addCommand(firstAlertCommand);
        if (commandProbe.getTimeout() != 333) return 93;
        commandProbe.addCommand(secondAlertCommand);
        if (commandProbe.getTimeout() != Alert.FOREVER) return 94;
        commandProbe.removeCommand(firstAlertCommand);
        if (commandProbe.getTimeout() != 333) return 95;
        commandProbe.removeCommand(secondAlertCommand);
        if (commandProbe.getTimeout() != 333) return 96;

        display.setCurrent((Displayable)null);
        if (display.getCurrent() != form) return 97;

        foreverAlert = new Alert("Alert", "Alert body", image,
                                 AlertType.INFO);
        foreverAlert.setTimeout(Alert.FOREVER);
        indicator = new Gauge(null, false, 10, 3);
        foreverAlert.setIndicator(indicator);
        boolean interactiveRejected = false;
        try {
            foreverAlert.setIndicator(gauge);
        } catch (IllegalArgumentException expected) {
            interactiveRejected = true;
        }
        if (!interactiveRejected || foreverAlert.getIndicator() != indicator) {
            return 92;
        }
        foreverAccepted = foreverAlert.getTimeout() == Alert.FOREVER;
        foreverAlert.setCommandListener(app);
        alert = foreverAlert;
        display.setCurrent(foreverAlert, form);
        return 0;
    }

    public static int verify() {
        if (!"acZ".equals(textBox.getString()) || textBox.size() != 3 ||
                textBox.getCaretPosition() != 1) return 1;
        if (!"-42".equals(constrainedBox.getString()) ||
                constrainedBox.getConstraints() !=
                    (TextField.NUMERIC | TextField.PASSWORD)) return 12;
        char[] copied = new char[4];
        if (textBox.getChars(copied) != 3 || copied[0] != 'a' ||
                copied[1] != 'c' || copied[2] != 'Z') return 2;
        if (imageItem.getImage() == null || imageItem.getImage().getWidth() != 2 ||
                imageItem.getImage().getHeight() != 3 ||
                !"image-alt".equals(imageItem.getAltText()) ||
                imageItem.getAppearanceMode() != 2) return 3;
        if (form.getTicker() != ticker ||
                !"ticker-two".equals(ticker.getString())) return 4;
        if (!foreverAccepted || alert.getTimeout() != 250 ||
                display.getCurrent() != form || !form.isShown() ||
                alert.isShown() || foreverAlert.getImage() == null ||
                foreverAlert.getImage().getWidth() != 2 ||
                foreverAlert.getImage().getHeight() != 3) return 5;
        if (!textFieldCharOpsPassed || !displayCapabilitiesPassed ||
                !choiceFontPassed || styledItem.getFont() != customFont ||
                foreverAlert.getIndicator() != indicator ||
                !formArrayPassed) return 13;
        styledItem.setFont(null);
        if (styledItem.getFont() == null) return 14;
        if (!"native-text".equals(textField.getString()) ||
                !"-42".equals(numericField.getString()) ||
                numericField.getConstraints() !=
                    (TextField.NUMERIC | TextField.PASSWORD) ||
                gauge.getValue() != 7 || !choice.isSelected(1) ||
                dateField.getDate() == null ||
                dateField.getDate().getTime() != 1700000000000L ||
                dateOnly.getDate() == null ||
                dateOnly.getDate().getTime() != 1700086400000L ||
                timeOnly.getDate() == null ||
                timeOnly.getDate().getTime() != 1700172800000L ||
                dateField.getInputMode() != DateField.DATE_TIME ||
                dateOnly.getInputMode() != DateField.DATE ||
                timeOnly.getInputMode() != DateField.TIME) return 6;
        if (stateCalls != 7 || listenerMutations != 1) return 7;
        if (itemCommandCalls != 1 || screenCommandCalls != 2) return 8;
        if (customItem.paintCalls < 3 || customItem.sizeCalls < 3 ||
                customItem.showCalls < 2 || customItem.hideCalls < 1 ||
                customItem.traverseCalls != 1 ||
                customItem.pointerPressCalls != 1 ||
                customItem.pointerReleaseCalls != 1 ||
                customItem.keyPressCalls != 1 ||
                customItem.keyReleaseCalls != 1 ||
                customItem.keyRepeatCalls != 1) return 9;
        if (form.size() != 13 || !"listener-mutated".equals(textField.getLabel())) {
            return 10;
        }
        if (spacer.getMinimumWidth() != 4 || spacer.getMinimumHeight() != 5) {
            return 11;
        }
        return 0;
    }

    public static void updateTicker() {
        ticker.setString("ticker-two");
    }

    public static void deleteEphemeral() {
        form.delete(ephemeralIndex);
    }

    public static void insertOrderProbe() {
        orderProbe = new StringItem("OrderProbe", "inserted");
        form.insert(1, orderProbe);
    }

    public static void deleteOrderProbe() {
        form.delete(1);
        orderProbe = null;
    }

    public static void removeStaleCommand() {
        form.removeCommand(staleCommand);
    }

    public static void showTimedAlert() {
        alert = new Alert("Timed", "Timed body", null, AlertType.WARNING);
        alert.setTimeout(250);
        alert.setCommandListener(app);
        display.setCurrent(alert, form);
    }

    public static void showLoadingThenData() {
        final Alert loading = new Alert(
            "Loading", "Loading...", null, AlertType.INFO);
        loading.setTimeout(Alert.FOREVER);
        loading.setIndicator(new Gauge(null, false, -1, 2));
        display.setCurrent(loading, form);
        display.callSerially(new Runnable() {
            public void run() {
                display.setCurrent(form);
            }
        });
    }

    public void itemStateChanged(Item item) {
        stateCalls++;
        if (listenerMutations == 0) {
            listenerMutations++;
            textField.setLabel("listener-mutated");
            form.append("listener mutation");
        }
    }

    public void commandAction(Command command, Item item) {
        itemCommandCalls++;
    }

    public void commandAction(Command command, Displayable displayable) {
        screenCommandCalls++;
        if (displayable instanceof Alert) {
            display.setCurrent(form);
        }
    }

    private static final class Probe extends CustomItem {
        int paintCalls;
        int sizeCalls;
        int showCalls;
        int hideCalls;
        int traverseCalls;
        int pointerPressCalls;
        int pointerReleaseCalls;
        int keyPressCalls;
        int keyReleaseCalls;
        int keyRepeatCalls;

        Probe(String label) { super(label); }
        protected int getMinContentWidth() { return 7; }
        protected int getMinContentHeight() { return 8; }
        protected int getPrefContentWidth(int height) { return 11; }
        protected int getPrefContentHeight(int width) { return 12; }
        protected void sizeChanged(int width, int height) { sizeCalls++; }
        protected void paint(Graphics graphics, int width, int height) {
            if (graphics != null && width >= 11 && height >= 12) paintCalls++;
        }
        protected boolean traverse(int direction, int viewportWidth,
                                   int viewportHeight, int[] visibleRect) {
            traverseCalls++;
            visibleRect[0] = 0;
            visibleRect[1] = 0;
            visibleRect[2] = viewportWidth;
            visibleRect[3] = viewportHeight;
            return true;
        }
        protected void pointerPressed(int x, int y) { pointerPressCalls++; }
        protected void pointerReleased(int x, int y) { pointerReleaseCalls++; }
        protected void keyPressed(int keyCode) { keyPressCalls++; }
        protected void keyReleased(int keyCode) { keyReleaseCalls++; }
        protected void keyRepeated(int keyCode) { keyRepeatCalls++; }
        protected void showNotify() { showCalls++; }
        protected void hideNotify() { hideCalls++; }
    }
}
